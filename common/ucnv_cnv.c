/*
******************************************************************************
*
*   Copyright (C) 1999-2003, International Business Machines
*   Corporation and others.  All Rights Reserved.
*
******************************************************************************
*
*   uconv_cnv.c:
*   Implements all the low level conversion functions
*   T_UnicodeConverter_{to,from}Unicode_$ConversionType
*
*   Change history:
*
*   06/29/2000  helena      Major rewrite of the callback APIs.
*/

#include "unicode/utypes.h"
#include "unicode/ucnv_err.h"
#include "unicode/ucnv.h"
#include "unicode/uset.h"
#include "ucnv_cnv.h"
#include "cmemory.h"

U_CFUNC void
ucnv_getCompleteUnicodeSet(const UConverter *cnv,
                   USet *set,
                   UConverterUnicodeSet which,
                   UErrorCode *pErrorCode) {
    uset_addRange(set, 0, 0x10ffff);
}

U_CFUNC void
ucnv_getNonSurrogateUnicodeSet(const UConverter *cnv,
                               USet *set,
                               UConverterUnicodeSet which,
                               UErrorCode *pErrorCode) {
    uset_addRange(set, 0, 0xd7ff);
    uset_addRange(set, 0xe000, 0x10ffff);
}

U_CFUNC void
ucnv_fromUWriteBytes(UConverter *cnv,
                     const char *bytes, int32_t length,
                     char **target, const char *targetLimit,
                     int32_t **offsets,
                     int32_t sourceIndex,
                     UErrorCode *pErrorCode) {
    char *t=*target;
    int32_t *o;

    /* write bytes */
    if(offsets==NULL || (o=*offsets)==NULL) {
        while(length>0 && t<targetLimit) {
            *t++=*bytes++;
            --length;
        }
    } else {
        /* output with offsets */
        while(length>0 && t<targetLimit) {
            *t++=*bytes++;
            *o++=sourceIndex;
            --length;
        }
        *offsets=o;
    }
    *target=t;

    /* write overflow */
    if(length>0) {
        if(cnv!=NULL) {
            t=(char *)cnv->charErrorBuffer;
            cnv->charErrorBufferLength=(int8_t)length;
            do {
                *t++=(uint8_t)*bytes++;
            } while(--length>0);
        }
        *pErrorCode=U_BUFFER_OVERFLOW_ERROR;
    }
}

U_CFUNC void
ucnv_toUWriteCodePoint(UConverter *cnv,
                       UChar32 c,
                       UChar **target, const UChar *targetLimit,
                       int32_t **offsets,
                       int32_t sourceIndex,
                       UErrorCode *pErrorCode) {
    UChar *t;
    int32_t *o;

    t=*target;

    if(t<targetLimit) {
        if(c<=0xffff) {
            *t++=(UChar)c;
            c=U_SENTINEL;
        } else /* c is a supplementary code point */ {
            *t++=U16_LEAD(c);
            c=U16_TRAIL(c);
            if(t<targetLimit) {
                *t++=(UChar)c;
                c=U_SENTINEL;
            }
        }

        /* write offsets */
        if(offsets!=NULL && (o=*offsets)!=NULL) {
            *o++=sourceIndex;
            if((*target+1)<t) {
                *o++=sourceIndex;
            }
            *offsets=o;
        }
    }

    *target=t;

    /* write overflow from c */
    if(c>=0) {
        if(cnv!=NULL) {
            int8_t i=0;
            U16_APPEND_UNSAFE(cnv->UCharErrorBuffer, i, c);
            cnv->UCharErrorBufferLength=i;
        }
        *pErrorCode=U_BUFFER_OVERFLOW_ERROR;
    }
}
