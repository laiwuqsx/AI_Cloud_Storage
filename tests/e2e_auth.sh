#!/bin/sh
set -eu

compose_file="docker/docker-compose.yml"
user_name="e2e_user_$(date +%s)"
nickname="e2e_nick_$(date +%s)"
password_md5="5f4dcc3b5aa765d61d8327deb882cf99"
wrong_password_md5="900150983cd24fb0d6963f7d28e17f72"

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

files_response=$(curl --silent --show-error --request POST \
    --header "Content-Type: application/json" \
    --data "{\"user\":\"$user_name\",\"token\":\"$token\"}" \
    http://localhost:8080/api/myfiles)
case "$files_response" in
    *'"code":0,"files":[]'*) ;;
    *) fail "empty file list response: $files_response" ;;
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
