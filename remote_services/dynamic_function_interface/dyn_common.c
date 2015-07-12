/**
 * Licensed under Apache License v2. See LICENSE for more information.
 */
#include "dyn_common.h"

#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#if defined(BSD) || defined(__APPLE__) 
#include "open_memstream.h"
#include "fmemopen.h"
#endif

static const int OK = 0;
static const int ERROR = 1;

DFI_SETUP_LOG(dynCommon)

int dynCommon_parseName(FILE *stream, char **result) {
    int status = OK;

    char *buf = NULL;
    size_t size = 0;
    int strLen = 0;
    FILE *name = open_memstream(&buf, &size);

    if (name != NULL) { 
        int c = getc(stream);
        while (isalnum(c) || c == '_') {
            fputc(c, name); 
            c = getc(stream);
            strLen += 1;
        }
        fflush(name);
        fclose(name);
        ungetc(c, stream);
    } else {
        status = ERROR;
        LOG_ERROR("Error creating mem stream for name. %s", strerror(errno));
    }

    if (status == OK) {
        if (strLen == 0) {
            status = ERROR;
            LOG_ERROR("Parsed empty name");
            free(buf);
        }
    }

    if (status == OK) {
       LOG_DEBUG("Parsed name '%s'", buf);
       *result = buf;
    } 

    return status;
}

int dynCommon_parseNameValue(FILE *stream, char **outName, char **outValue) {
    int status = OK;
    char *name = NULL;
    char *value = NULL;

    status = dynCommon_parseName(stream, &name);
    if (status == OK) {
        status = dynCommon_eatChar(stream, '=');
    }
    if (status == OK) {
        status = dynCommon_parseName(stream, &value); //TODO use different more lenient function?
    }

    if (status == OK) {
        *outName = name;
        *outValue = value;
    } else {
        if (name != NULL) {
            free(name);
        }
        if (value != NULL) {
            free(value);
        }
    }
    return status;
}

int dynCommon_eatChar(FILE *stream, int expected) {
    int status = OK;
    int c = fgetc(stream);
    if (c != expected) {
        status = ERROR;
        LOG_ERROR("Error parsing, expected token '%c' got '%c'", expected, c);
    }
    return status;
}
