#include <mysql/mysql.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "file_repository.h"

static MYSQL *connect_database(void)
{
    MYSQL *conn = mysql_init(NULL);

    if (!conn) return NULL;
    if (!mysql_real_connect(conn, getenv("MYSQL_HOST") ? getenv("MYSQL_HOST") : "127.0.0.1",
                            getenv("MYSQL_USER") ? getenv("MYSQL_USER") : "root",
                            getenv("MYSQL_PASSWORD") ? getenv("MYSQL_PASSWORD") : "",
                            getenv("MYSQL_DATABASE") ? getenv("MYSQL_DATABASE") : "ai_cloud_storage",
                            3306, NULL, 0)) {
        mysql_close(conn);
        return NULL;
    }
    return conn;
}

static int verify_and_cleanup(const char *user, const char *md5, const char *storage_key)
{
    MYSQL *conn = NULL;
    MYSQL_RES *result_set = NULL;
    MYSQL_ROW row;
    char sql[1024];
    int verified = 0;

    conn = connect_database();
    if (!conn) return 0;
    snprintf(sql, sizeof(sql),
             "SELECT f.reference_count, COUNT(u.id), f.storage_key "
             "FROM file_info f LEFT JOIN user_file_list u "
             "ON u.md5 = f.md5 AND u.user_name = '%s' "
             "WHERE f.md5 = '%s' GROUP BY f.id",
             user, md5);
    if (mysql_query(conn, sql) == 0 && (result_set = mysql_store_result(conn)) != NULL &&
        (row = mysql_fetch_row(result_set)) != NULL &&
        row[0] && strcmp(row[0], "1") == 0 &&
        row[1] && strcmp(row[1], "1") == 0 &&
        row[2] && strcmp(row[2], storage_key) == 0) verified = 1;
    if (result_set) mysql_free_result(result_set);

    snprintf(sql, sizeof(sql),
             "DELETE FROM user_file_list WHERE user_name = '%s' AND md5 = '%s'", user, md5);
    if (mysql_query(conn, sql) != 0) verified = 0;
    snprintf(sql, sizeof(sql), "DELETE FROM file_info WHERE md5 = '%s'", md5);
    if (mysql_query(conn, sql) != 0) verified = 0;
    mysql_close(conn);
    return verified;
}

int main(int argc, char **argv)
{
    NewFileRecord record;
    RecordNewFileResult first_result, duplicate_result;
    char storage_key[128], url[256];
    int verified, confirmed, wrong_key_absent, owner_found, stranger_absent;

    if (argc != 3 || strlen(argv[1]) > 32 || strlen(argv[2]) != 32) return 2;
    snprintf(storage_key, sizeof(storage_key), "probe/%s", argv[2]);
    snprintf(url, sizeof(url), "http://storage.local/probe/%s", argv[2]);
    record.user_name = argv[1];
    record.md5 = argv[2];
    record.file_name = "transaction-probe.txt";
    record.storage_key = storage_key;
    record.url = url;
    record.type = "txt";
    record.size = 42;

    first_result = record_new_file_upload(&record);
    duplicate_result = record_new_file_upload(&record);
    confirmed = confirm_new_file_upload(record.user_name, record.md5, record.storage_key);
    wrong_key_absent = confirm_new_file_upload(record.user_name, record.md5, "probe/wrong-key");
    owner_found = user_owns_file(record.user_name, record.md5);
    stranger_absent = user_owns_file("probe_stranger", record.md5);
    verified = verify_and_cleanup(record.user_name, record.md5, record.storage_key);
    if (first_result != RECORD_NEW_FILE_CREATED ||
        duplicate_result != RECORD_NEW_FILE_PHYSICAL_CONFLICT ||
        confirmed != 1 || wrong_key_absent != 0 || owner_found != 1 ||
        stranger_absent != 0 || !verified) {
        fprintf(stderr,
                "upload repository probe failed: first=%d duplicate=%d confirmed=%d "
                "absent=%d owner=%d stranger=%d verified=%d\n",
                first_result, duplicate_result, confirmed, wrong_key_absent,
                owner_found, stranger_absent, verified);
        return 1;
    }
    puts("upload repository probe passed");
    return 0;
}
