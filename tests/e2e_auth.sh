#!/bin/sh
set -eu

compose_file="docker/docker-compose.yml"
app_port="${APP_PORT:-8080}"
base_url="http://localhost:$app_port"
user_name="e2e_user_$(date +%s)"
nickname="e2e_nick_$(date +%s)"
password_md5="5f4dcc3b5aa765d61d8327deb882cf99"
wrong_password_md5="900150983cd24fb0d6963f7d28e17f72"
shared_md5="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
missing_md5="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
transaction_md5=$(printf "%032d" "$(date +%s)")
upload_fixture=$(mktemp)
downloaded_fixture=$(mktemp)
race_fixture=$(mktemp)
race_response_file_a=$(mktemp)
race_response_file_b=$(mktemp)

cleanup() {
    rm -f "$upload_fixture" "$downloaded_fixture" "$race_fixture" \
        "$race_response_file_a" "$race_response_file_b"
}

trap cleanup EXIT HUP INT TERM

printf 'FastDFS upload fixture for %s\n' "$user_name" > "$upload_fixture"
upload_size=$(wc -c < "$upload_fixture" | tr -d '[:space:]')
if command -v md5sum >/dev/null 2>&1; then
    upload_md5=$(md5sum "$upload_fixture" | awk '{print $1}')
else
    upload_md5=$(md5 -q "$upload_fixture")
fi

race_suffix="$(date +%s)_$$"
race_user_a="race_a_$race_suffix"
race_user_b="race_b_$race_suffix"
race_nickname_a="race_nick_a_$race_suffix"
race_nickname_b="race_nick_b_$race_suffix"
printf 'Concurrent FastDFS fixture for %s\n' "$race_suffix" > "$race_fixture"
race_size=$(wc -c < "$race_fixture" | tr -d '[:space:]')
if command -v md5sum >/dev/null 2>&1; then
    race_md5=$(md5sum "$race_fixture" | awk '{print $1}')
else
    race_md5=$(md5 -q "$race_fixture")
fi

fail() {
    echo "e2e auth test failed: $1" >&2
    exit 1
}

echo "Starting authentication development stack..."
docker compose -f "$compose_file" up -d --build

attempt=1
until curl --silent --fail "$base_url/healthz" >/dev/null; do
    if [ "$attempt" -ge 30 ]; then
        fail "nginx did not become healthy"
    fi
    attempt=$((attempt + 1))
    sleep 1
done

docker compose -f "$compose_file" exec -T mysql sh -c \
    'mysql -uroot -p"$MYSQL_ROOT_PASSWORD" ai_cloud_storage' \
    < sql/migrations/001_storage_cleanup_job.sql || \
    fail "storage cleanup migration"

register_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"nickname\":\"$nickname\",\"password\":\"$password_md5\"}" \
    "$base_url/api/reg")
case "$register_response" in
    *'"code":0'*) ;;
    *) fail "registration response: $register_response" ;;
esac

login_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"password\":\"$password_md5\"}" \
    "$base_url/api/login")
case "$login_response" in
    *'"code":0'*) ;;
    *) fail "login response: $login_response" ;;
esac

token=$(printf "%s" "$login_response" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
[ -n "$token" ] || fail "response did not contain a token"

stored_user=$(docker compose -f "$compose_file" exec -T redis \
    redis-cli --raw GET "token:$token")
[ "$stored_user" = "$user_name" ] || fail "Redis session does not match user"

upload_response=$(curl --silent --show-error --request POST \
    --header "X-Upload-User: $user_name" \
    --header "X-Upload-Token: $token" \
    --header "X-Upload-MD5: $upload_md5" \
    --header "X-Upload-Size: $upload_size" \
    --form "file=@$upload_fixture;filename=fastdfs-e2e.txt;type=text/plain" \
    "$base_url/api/upload")
case "$upload_response" in
    *'"code":0'*'"msg":"upload complete"'*'"url":'*) ;;
    *) fail "real FastDFS upload response: $upload_response" ;;
esac

upload_url=$(printf "%s" "$upload_response" | sed -n 's/.*"url":"\([^"]*\)".*/\1/p')
[ -n "$upload_url" ] || fail "upload response did not contain a URL"
curl --silent --show-error --fail "$upload_url" --output "$downloaded_fixture" || \
    fail "uploaded file URL was not downloadable: $upload_url"
cmp -s "$upload_fixture" "$downloaded_fixture" || \
    fail "downloaded FastDFS object differs from uploaded bytes"

upload_db_row=$(docker compose -f "$compose_file" exec -T mysql sh -c \
    'mysql -uroot -p"$MYSQL_ROOT_PASSWORD" --batch --skip-column-names ai_cloud_storage -e "SELECT CONCAT(f.storage_key, CHAR(124), f.reference_count, CHAR(124), COUNT(u.id)) FROM file_info f LEFT JOIN user_file_list u ON u.md5 = f.md5 WHERE f.md5 = '\''$1'\'' GROUP BY f.storage_key, f.reference_count;"' \
    sh "$upload_md5")
case "$upload_db_row" in
    group1/M00/*'|1|1') ;;
    *) fail "uploaded file database state: $upload_db_row" ;;
esac

upload_temp_files=$(docker compose -f "$compose_file" exec -T fastcgi_app \
    find /tmp -maxdepth 1 -name 'ai-cloud-upload-*' -print)
[ -z "$upload_temp_files" ] || fail "temporary upload file was not cleaned"

race_register_a=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$race_user_a\",\"nickname\":\"$race_nickname_a\",\"password\":\"$password_md5\"}" \
    "$base_url/api/reg")
case "$race_register_a" in
    *'"code":0'*) ;;
    *) fail "race user A registration response: $race_register_a" ;;
esac

race_register_b=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$race_user_b\",\"nickname\":\"$race_nickname_b\",\"password\":\"$password_md5\"}" \
    "$base_url/api/reg")
case "$race_register_b" in
    *'"code":0'*) ;;
    *) fail "race user B registration response: $race_register_b" ;;
esac

race_login_a=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$race_user_a\",\"password\":\"$password_md5\"}" \
    "$base_url/api/login")
race_token_a=$(printf "%s" "$race_login_a" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
[ -n "$race_token_a" ] || fail "race user A login response: $race_login_a"

race_login_b=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$race_user_b\",\"password\":\"$password_md5\"}" \
    "$base_url/api/login")
race_token_b=$(printf "%s" "$race_login_b" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
[ -n "$race_token_b" ] || fail "race user B login response: $race_login_b"

physical_files_before=$(docker compose -f "$compose_file" exec -T storage sh -c \
    'find /data/fastdfs/storage/data -type f | wc -l' | tr -d '[:space:]')

curl --silent --show-error --request POST \
    --header "X-Upload-User: $race_user_a" \
    --header "X-Upload-Token: $race_token_a" \
    --header "X-Upload-MD5: $race_md5" \
    --header "X-Upload-Size: $race_size" \
    --form "file=@$race_fixture;filename=race-a.txt;type=text/plain" \
    "$base_url/api/upload" > "$race_response_file_a" &
race_pid_a=$!

curl --silent --show-error --request POST \
    --header "X-Upload-User: $race_user_b" \
    --header "X-Upload-Token: $race_token_b" \
    --header "X-Upload-MD5: $race_md5" \
    --header "X-Upload-Size: $race_size" \
    --form "file=@$race_fixture;filename=race-b.txt;type=text/plain" \
    "$base_url/api/upload" > "$race_response_file_b" &
race_pid_b=$!

wait "$race_pid_a" || fail "race user A upload request failed"
wait "$race_pid_b" || fail "race user B upload request failed"
race_response_a=$(cat "$race_response_file_a")
race_response_b=$(cat "$race_response_file_b")
case "$race_response_a" in
    *'"code":0'*'"url":'*) ;;
    *) fail "race user A upload response: $race_response_a" ;;
esac
case "$race_response_b" in
    *'"code":0'*'"url":'*) ;;
    *) fail "race user B upload response: $race_response_b" ;;
esac
race_url_a=$(printf "%s" "$race_response_a" | sed -n 's/.*"url":"\([^"]*\)".*/\1/p')
race_url_b=$(printf "%s" "$race_response_b" | sed -n 's/.*"url":"\([^"]*\)".*/\1/p')
[ -n "$race_url_a" ] && [ "$race_url_a" = "$race_url_b" ] || \
    fail "concurrent uploads did not converge on one URL: $race_url_a / $race_url_b"

race_db_row=$(docker compose -f "$compose_file" exec -T mysql sh -c \
    'mysql -uroot -p"$MYSQL_ROOT_PASSWORD" --batch --skip-column-names ai_cloud_storage -e "SELECT CONCAT(f.reference_count, CHAR(124), COUNT(u.id)) FROM file_info f JOIN user_file_list u ON u.md5 = f.md5 WHERE f.md5 = '\''$1'\'' GROUP BY f.reference_count;"' \
    sh "$race_md5")
[ "$race_db_row" = "2|2" ] || fail "concurrent upload database state: $race_db_row"

physical_files_after=$(docker compose -f "$compose_file" exec -T storage sh -c \
    'find /data/fastdfs/storage/data -type f | wc -l' | tr -d '[:space:]')
[ "$physical_files_after" -eq $((physical_files_before + 1)) ] || \
    fail "duplicate FastDFS object was not cleaned: before=$physical_files_before after=$physical_files_after"

docker compose -f "$compose_file" exec -T fastcgi_app \
    /app/bin_cgi/upload_repository_probe "$user_name" "$transaction_md5" || \
    fail "first-upload database transaction probe"

docker compose -f "$compose_file" exec -T fastcgi_app \
    /app/bin_cgi/cleanup_repository_probe "$transaction_md5" || \
    fail "storage cleanup repository probe"

cleanup_worker_key=$(docker compose -f "$compose_file" exec -T fastcgi_app sh -c \
    'temporary_file=$(mktemp); printf "cleanup worker fixture\n" > "$temporary_file"; fdfs_upload_file /etc/fdfs/client.conf "$temporary_file"; rm -f "$temporary_file"')
case "$cleanup_worker_key" in
    group1/M00/*) ;;
    *) fail "cleanup worker fixture upload: $cleanup_worker_key" ;;
esac
curl --silent --show-error --fail \
    "$base_url/storage/$cleanup_worker_key" >/dev/null || \
    fail "cleanup worker fixture was not downloadable"
docker compose -f "$compose_file" exec -T mysql sh -c \
    'mysql -uroot -p"$MYSQL_ROOT_PASSWORD" ai_cloud_storage -e "INSERT INTO storage_cleanup_job (storage_key, reason, last_error) VALUES ('\''$1'\'', '\''e2e_manual_retry'\'', '\''fixture'\'');"' \
    sh "$cleanup_worker_key" || fail "cleanup worker fixture enqueue"
docker compose -f "$compose_file" exec -T fastcgi_app \
    /app/bin_cgi/cleanup_worker 1 || fail "cleanup worker execution"
cleanup_worker_status=$(docker compose -f "$compose_file" exec -T mysql sh -c \
    'mysql -uroot -p"$MYSQL_ROOT_PASSWORD" --batch --skip-column-names ai_cloud_storage -e "SELECT status FROM storage_cleanup_job WHERE storage_key = '\''$1'\'';"' \
    sh "$cleanup_worker_key")
[ "$cleanup_worker_status" = "done" ] || \
    fail "cleanup worker status: $cleanup_worker_status"
if curl --silent --fail "$base_url/storage/$cleanup_worker_key" >/dev/null; then
    fail "cleanup worker did not remove the FastDFS object"
fi
docker compose -f "$compose_file" exec -T mysql sh -c \
    'mysql -uroot -p"$MYSQL_ROOT_PASSWORD" ai_cloud_storage -e "DELETE FROM storage_cleanup_job WHERE storage_key = '\''$1'\'';"' \
    sh "$cleanup_worker_key" || fail "cleanup worker fixture database cleanup"

missing_file_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\",\"md5\":\"$missing_md5\",\"file_name\":\"missing-demo.txt\"}" \
    "$base_url/api/md5")
case "$missing_file_response" in
    *'"code":1'*'"msg":"physical file not found"'*) ;;
    *) fail "missing physical file response: $missing_file_response" ;;
esac

invalid_md5_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\",\"md5\":\"not-an-md5\",\"file_name\":\"invalid-demo.txt\"}" \
    "$base_url/api/md5")
case "$invalid_md5_response" in
    *'"code":3'*'"msg":"invalid instant upload request"'*) ;;
    *) fail "invalid instant upload request response: $invalid_md5_response" ;;
esac

docker compose -f "$compose_file" exec -T mysql sh -c \
    'mysql -uroot -p"$MYSQL_ROOT_PASSWORD" ai_cloud_storage -e "INSERT INTO file_info (md5, storage_key, url, size, type) VALUES ('\''aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'\'', '\''demo/shared-demo.txt'\'', '\''http://storage.local/shared-demo.txt'\'', 42, '\''txt'\'') ON DUPLICATE KEY UPDATE url = VALUES(url);"'

instant_upload_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\",\"md5\":\"$shared_md5\",\"file_name\":\"shared-demo.txt\"}" \
    "$base_url/api/md5")
case "$instant_upload_response" in
    *'"code":0'*) ;;
    *) fail "instant upload response: $instant_upload_response" ;;
esac

share_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\",\"md5\":\"$shared_md5\"}" \
    "$base_url/api/dealfile?cmd=share")
case "$share_response" in
    *'"code":0'*) ;;
    *) fail "share response: $share_response" ;;
esac

files_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\"}" \
    "$base_url/api/myfiles")
case "$files_response" in
    *'"file_name":"shared-demo.txt","url":'*'"shared_status":1'*) ;;
    *) fail "file list response: $files_response" ;;
esac

duplicate_share_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\",\"md5\":\"$shared_md5\"}" \
    "$base_url/api/dealfile?cmd=share")
case "$duplicate_share_response" in
    *'"code":5'*) ;;
    *) fail "duplicate share response: $duplicate_share_response" ;;
esac

unshare_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\",\"md5\":\"$shared_md5\"}" \
    "$base_url/api/dealfile?cmd=unshare")
case "$unshare_response" in
    *'"code":0'*) ;;
    *) fail "unshare response: $unshare_response" ;;
esac

files_after_unshare=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\"}" \
    "$base_url/api/myfiles")
case "$files_after_unshare" in
    *'"file_name":"shared-demo.txt","url":'*'"shared_status":0'*) ;;
    *) fail "file list after unshare: $files_after_unshare" ;;
esac

duplicate_unshare_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\",\"md5\":\"$shared_md5\"}" \
    "$base_url/api/dealfile?cmd=unshare")
case "$duplicate_unshare_response" in
    *'"code":1'*) ;;
    *) fail "duplicate unshare response: $duplicate_unshare_response" ;;
esac

duplicate_upload_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\",\"md5\":\"$shared_md5\",\"file_name\":\"shared-demo.txt\"}" \
    "$base_url/api/md5")
case "$duplicate_upload_response" in
    *'"code":5'*) ;;
    *) fail "duplicate instant upload response: $duplicate_upload_response" ;;
esac

delete_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\",\"md5\":\"$shared_md5\"}" \
    "$base_url/api/dealfile?cmd=del")
case "$delete_response" in
    *'"code":0'*) ;;
    *) fail "delete response: $delete_response" ;;
esac

files_after_delete=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\"}" \
    "$base_url/api/myfiles")
case "$files_after_delete" in
    *"\"md5\":\"$shared_md5\""*) fail "deleted shared file is still listed: $files_after_delete" ;;
    *"\"md5\":\"$upload_md5\""*) ;;
    *) fail "real uploaded file missing after unrelated delete: $files_after_delete" ;;
esac

logout_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\"}" \
    "$base_url/api/logout")
case "$logout_response" in
    *'"code":0'*) ;;
    *) fail "logout response: $logout_response" ;;
esac

stored_user_after_logout=$(docker compose -f "$compose_file" exec -T redis \
    redis-cli --raw GET "token:$token")
[ -z "$stored_user_after_logout" ] || fail "Redis session still exists after logout"

files_after_logout=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\"}" \
    "$base_url/api/myfiles")
case "$files_after_logout" in
    *'"code":4'*) ;;
    *) fail "old token accepted after logout: $files_after_logout" ;;
esac

duplicate_logout_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\"}" \
    "$base_url/api/logout")
case "$duplicate_logout_response" in
    *'"code":0'*) ;;
    *) fail "duplicate logout response: $duplicate_logout_response" ;;
esac

failed_login_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"password\":\"$wrong_password_md5\"}" \
    "$base_url/api/login")
case "$failed_login_response" in
    *'"code":2'*) ;;
    *) fail "wrong-password response: $failed_login_response" ;;
esac

echo "e2e auth test passed for $user_name"
