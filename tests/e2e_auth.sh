#!/bin/sh
set -eu

compose_file="docker/docker-compose.yml"
user_name="e2e_user_$(date +%s)"
nickname="e2e_nick_$(date +%s)"
password_md5="5f4dcc3b5aa765d61d8327deb882cf99"
wrong_password_md5="900150983cd24fb0d6963f7d28e17f72"
shared_md5="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

fail() {
    echo "e2e auth test failed: $1" >&2
    exit 1
}

echo "Starting authentication development stack..."
docker compose -f "$compose_file" up -d --build

attempt=1
until curl --silent --fail http://localhost:8080/healthz >/dev/null; do
    if [ "$attempt" -ge 30 ]; then
        fail "nginx did not become healthy"
    fi
    attempt=$((attempt + 1))
    sleep 1
done

register_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"nickname\":\"$nickname\",\"password\":\"$password_md5\"}" \
    http://localhost:8080/api/reg)
case "$register_response" in
    *'"code":0'*) ;;
    *) fail "registration response: $register_response" ;;
esac

login_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"password\":\"$password_md5\"}" \
    http://localhost:8080/api/login)
case "$login_response" in
    *'"code":0'*) ;;
    *) fail "login response: $login_response" ;;
esac

token=$(printf "%s" "$login_response" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
[ -n "$token" ] || fail "response did not contain a token"

stored_user=$(docker compose -f "$compose_file" exec -T redis \
    redis-cli --raw GET "token:$token")
[ "$stored_user" = "$user_name" ] || fail "Redis session does not match user"

docker compose -f "$compose_file" exec -T mysql sh -c \
    'mysql -uroot -p"$MYSQL_ROOT_PASSWORD" ai_cloud_storage -e "INSERT INTO file_info (md5, storage_key, url, size, type) VALUES ('\''aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'\'', '\''demo/shared-demo.txt'\'', '\''http://storage.local/shared-demo.txt'\'', 42, '\''txt'\'') ON DUPLICATE KEY UPDATE url = VALUES(url);"'

instant_upload_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\",\"md5\":\"$shared_md5\",\"file_name\":\"shared-demo.txt\"}" \
    http://localhost:8080/api/md5)
case "$instant_upload_response" in
    *'"code":0'*) ;;
    *) fail "instant upload response: $instant_upload_response" ;;
esac

share_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\",\"md5\":\"$shared_md5\"}" \
    "http://localhost:8080/api/dealfile?cmd=share")
case "$share_response" in
    *'"code":0'*) ;;
    *) fail "share response: $share_response" ;;
esac

files_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\"}" \
    http://localhost:8080/api/myfiles)
case "$files_response" in
    *'"file_name":"shared-demo.txt","url":'*'"shared_status":1'*) ;;
    *) fail "file list response: $files_response" ;;
esac

duplicate_share_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\",\"md5\":\"$shared_md5\"}" \
    "http://localhost:8080/api/dealfile?cmd=share")
case "$duplicate_share_response" in
    *'"code":5'*) ;;
    *) fail "duplicate share response: $duplicate_share_response" ;;
esac

unshare_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\",\"md5\":\"$shared_md5\"}" \
    "http://localhost:8080/api/dealfile?cmd=unshare")
case "$unshare_response" in
    *'"code":0'*) ;;
    *) fail "unshare response: $unshare_response" ;;
esac

files_after_unshare=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\"}" \
    http://localhost:8080/api/myfiles)
case "$files_after_unshare" in
    *'"file_name":"shared-demo.txt","url":'*'"shared_status":0'*) ;;
    *) fail "file list after unshare: $files_after_unshare" ;;
esac

duplicate_unshare_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\",\"md5\":\"$shared_md5\"}" \
    "http://localhost:8080/api/dealfile?cmd=unshare")
case "$duplicate_unshare_response" in
    *'"code":1'*) ;;
    *) fail "duplicate unshare response: $duplicate_unshare_response" ;;
esac

duplicate_upload_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\",\"md5\":\"$shared_md5\",\"file_name\":\"shared-demo.txt\"}" \
    http://localhost:8080/api/md5)
case "$duplicate_upload_response" in
    *'"code":5'*) ;;
    *) fail "duplicate instant upload response: $duplicate_upload_response" ;;
esac

delete_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\",\"md5\":\"$shared_md5\"}" \
    "http://localhost:8080/api/dealfile?cmd=del")
case "$delete_response" in
    *'"code":0'*) ;;
    *) fail "delete response: $delete_response" ;;
esac

files_after_delete=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\"}" \
    http://localhost:8080/api/myfiles)
case "$files_after_delete" in
    *'"code":0,"files":[]'*) ;;
    *) fail "file list after delete: $files_after_delete" ;;
esac

failed_login_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"password\":\"$wrong_password_md5\"}" \
    http://localhost:8080/api/login)
case "$failed_login_response" in
    *'"code":2'*) ;;
    *) fail "wrong-password response: $failed_login_response" ;;
esac

echo "e2e auth test passed for $user_name"
