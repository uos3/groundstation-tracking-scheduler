#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <inttypes.h>

#include <libpq-fe.h> 
#include <curl/curl.h>

#include "tle.h"
#include "libpredict/predict.h"

void tle_update_http(PGconn *conn)
{
    PGresult *res = PQexec(conn, "SELECT id,name,tle_source_url,tle_spacecraft_uri FROM spacecraft WHERE tle_source_type = 'http' AND (tle_epoch = timestamp 'epoch' OR tle_updated < NOW()-INTERVAL '15 minutes');");
    if(PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        printf("Error: TLE HTTP Update Select failed!\n");
        return;
    }
    int rows = PQntuples(res);
    if(rows == 0)
    {
        printf(" * Up to date\n");
        PQclear(res);
        return;
    }
    for(int i=0; i<rows; i++) {

        char tle_line[2][150];
        char *tle_lines[2];
        tle_lines[0] = &tle_line[0][0];
        tle_lines[1] = &tle_line[1][0];

        int row_id = atoi(PQgetvalue(res, i, 0));
        char *tle_source_url = PQgetvalue(res, i, 2);
        char *tle_source_uri = PQgetvalue(res, i, 3);

        printf(" * %s\n",PQgetvalue(res, i, 1));

        if(!tle_download_http(tle_source_url, tle_source_uri, tle_line[0], tle_line[1]))
        {
          fprintf(stderr, " * * Error: TLE Download failed!\n");
          continue;
        }

        predict_orbital_elements_t tle;
        struct predict_sgp4 sgp;
        struct predict_sdp4 sdp;

        if(!predict_parse_tle(&tle, tle_lines[0], tle_lines[1], &sgp, &sdp))
        {
          fprintf(stderr, " * * Error: Parsing TLE failed!\n");
          continue;
        }

        char sql_stmt[200];
        snprintf(sql_stmt,200
            , "UPDATE spacecraft SET tle_lines_0 = $1, tle_lines_1 = $2, tle_updated = NOW(), tle_epoch = (make_timestamp(%d, 1,1,0,0,0) + interval '%f days') WHERE id=%d;"
            , (2000+tle.epoch_year)
            , (tle.epoch_day-1)
            , row_id
        );
        PGresult *sres = PQexecParams(conn, sql_stmt, 2, NULL, (const char * const*)tle_lines, NULL, NULL, 0);
        if(PQresultStatus(sres) != PGRES_COMMAND_OK)
        {
            fprintf(stderr, " * * Error: TLE Database Update failed! : %s\n", sql_stmt);
        }
        PQclear(sres);
    }

    PQclear(res);
}

void tle_update_spacetrack(PGconn *conn, char *user, char *password)
{
    PGresult *res = PQexec(conn, "SELECT id,name,tle_source_url,tle_spacecraft_uri FROM spacecraft WHERE tle_source_type = 'spacetrack' AND (tle_epoch = timestamp 'epoch' OR tle_updated < NOW()-INTERVAL '15 minutes');");
    if(PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        printf("Error: TLE Spacetrack Update Select failed!\n");
        return;
    }
    int rows = PQntuples(res);
    if(rows == 0)
    {
        printf(" * Up to date\n");
        PQclear(res);
        return;
    }
    for(int i=0; i<rows; i++) {
        char tle_line[2][150];
        char *tle_lines[2];
        tle_lines[0] = &tle_line[0][0];
        tle_lines[1] = &tle_line[1][0];

        printf(" * %s\n",PQgetvalue(res, i, 1));

        int row_id = atoi(PQgetvalue(res, i, 0));
        char *tle_source_url = PQgetvalue(res, i, 2);
        char *tle_source_uri = PQgetvalue(res, i, 3);

        if(!tle_download_spacetrack(tle_source_url, user, password, tle_source_uri, tle_line[0], tle_line[1]))
        {
          fprintf(stderr, " * * Error: TLE Download failed!\n");
          continue;
        }

        predict_orbital_elements_t tle;
        struct predict_sgp4 sgp;
        struct predict_sdp4 sdp;

        if(!predict_parse_tle(&tle, tle_lines[0], tle_lines[1], &sgp, &sdp))
        {
          fprintf(stderr, " * * Error: Parsing TLE failed!\n");
          continue;
        }

        char sql_stmt[200];
        snprintf(sql_stmt,200
            , "UPDATE spacecraft SET tle_lines_0 = $1, tle_lines_1 = $2, tle_updated = NOW(), tle_epoch = (make_timestamp(%d, 1,1,0,0,0) + interval '%f days') WHERE id=%d;"
            , (2000+tle.epoch_year)
            , (tle.epoch_day-1)
            , row_id
        );
        PGresult *sres = PQexecParams(conn, sql_stmt, 2, NULL, (const char * const*)tle_lines, NULL, NULL, 0);
        if(PQresultStatus(sres) != PGRES_COMMAND_OK)
        {
            fprintf(stderr, " * * Error: TLE Database Update failed! : %s\n", sql_stmt);
        }
        PQclear(sres);
    }

    PQclear(res);
}
typedef struct curl_buffer {
  char *memory;
  size_t size;
} curl_buffer;
 
static size_t curl_buffer_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t realsize = size * nmemb;
  curl_buffer *mem = (curl_buffer *)userp;
 
  mem->memory = realloc(mem->memory, mem->size + realsize + 1);
  if(mem->memory == NULL) {
    printf("not enough memory (realloc returned NULL)\n");
    return 0;
  }
 
  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;
 
  return realsize;
}

static bool tle_extract(curl_buffer *tle_buffer, char *craft_uri, char *tle_0, char *tle_1)
{
  char *tle_line;
  const char newline[2] = "\n";

  tle_buffer->memory[tle_buffer->size]='\0';
  tle_line = strtok(tle_buffer->memory, newline);

  while (tle_line != NULL)
  {
    if(0 == strncmp(tle_line,craft_uri,strlen(craft_uri)))
    {
      /* Found spacecraft */
      strcpy(tle_0,strtok(NULL, newline));
      strcpy(tle_1,strtok(NULL, newline));

      return true;
    }
    tle_line = strtok(NULL, newline);
  }
  /* Failed to find spacecraft */
  return false;
}
 
bool tle_download_http(char *url, char *craft_uri, char *tle_0, char *tle_1)
{
  CURL *curl_handle;
  CURLcode res;
 
  curl_buffer tle_buffer;
 
  tle_buffer.memory = malloc(1);  /* will be grown as needed by the realloc above */ 
  tle_buffer.size = 0;    /* no data at this point */ 
 
  curl_global_init(CURL_GLOBAL_ALL);
  curl_handle = curl_easy_init();
  curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "gss/0.1");
  curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
   
  curl_easy_setopt(curl_handle, CURLOPT_URL, url);

  curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, curl_buffer_cb);
  curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&tle_buffer);
 
  res = curl_easy_perform(curl_handle);

  curl_easy_cleanup(curl_handle);
  curl_global_cleanup();
 
  if(res != CURLE_OK)
  {
    fprintf(stderr, " * * * Error: Curl failed with error: %s\n",
      curl_easy_strerror(res));

  	free(tle_buffer.memory);
  	return false;
  }

  long http_code = 0;
  curl_easy_getinfo (curl_handle, CURLINFO_RESPONSE_CODE, &http_code);
  if(http_code != 200)
  {
    fprintf(stderr, " * * * Error: Download failed with HTTP status: %ld\n",
      http_code);

    free(tle_buffer.memory);
    return false;
  }

	if(!tle_extract(&tle_buffer, craft_uri, tle_0, tle_1))
	{
	  fprintf(stderr, " * * * Error: Spacecraft URI not found in downloaded TLE\n");

	  free(tle_buffer.memory);
	  return false;
	}
 
  free(tle_buffer.memory);
  return true;
}

bool tle_download_spacetrack(char *url, char *user, char *password, char *craft_uri, char *tle_0, char *tle_1)
{
  CURL *curl_handle;
  CURLcode res;
  char postData[300];
 
  curl_buffer tle_buffer;
 
  tle_buffer.memory = malloc(1);  /* will be grown as needed by the realloc above */ 
  tle_buffer.size = 0;    /* no data at this point */ 
 
  curl_global_init(CURL_GLOBAL_ALL);
  curl_handle = curl_easy_init();
  curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "gss/0.1");
  curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
   
  curl_easy_setopt(curl_handle, CURLOPT_URL, "https://www.space-track.org/ajaxauth/login");

  snprintf(postData, 300, "identity=%s&password=%s&query=%s", user, password, url);
  curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, postData);

  curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, curl_buffer_cb);
  curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&tle_buffer);
 
  res = curl_easy_perform(curl_handle);

  curl_easy_cleanup(curl_handle);
  curl_global_cleanup();
 
  if(res != CURLE_OK)
  {
    fprintf(stderr, " * * * Error: Curl failed with error: %s\n",
      curl_easy_strerror(res));

    free(tle_buffer.memory);
    return false;
  }

  /* Note that checking the HTTP code doesn't catch an incorrect password with Celestrak! */
  long http_code = 0;
  curl_easy_getinfo (curl_handle, CURLINFO_RESPONSE_CODE, &http_code);
  if(http_code != 200)
  {
    fprintf(stderr, " * * * Error: Download failed with HTTP status: %ld\n",
      http_code);

    free(tle_buffer.memory);
    return false;
  }
  
  if(!tle_extract(&tle_buffer, craft_uri, tle_0, tle_1))
  {
    fprintf(stderr, " * * * Error: Spacecraft URI not found in downloaded TLE\n");
    fprintf(stderr, "        Note: SpaceTrack User/Password may be incorrect!\n");

    free(tle_buffer.memory);
    return false;
  }
 
  free(tle_buffer.memory);
  return true;
}

