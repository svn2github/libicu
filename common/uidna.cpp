/*
 *******************************************************************************
 *
 *   Copyright (C) 2003, International Business Machines
 *   Corporation and others.  All Rights Reserved.
 *
 *******************************************************************************
 *   file name:  uidna.cpp
 *   encoding:   US-ASCII
 *   tab size:   8 (not used)
 *   indentation:4
 *
 *   created on: 2003feb1
 *   created by: Ram Viswanadha
 */

#include "unicode/utypes.h"

#if !UCONFIG_NO_IDNA

#include "unicode/uidna.h"
#include "unicode/ustring.h"
#include "strprep.h"
#include "punycode.h"
#include "ustr_imp.h"
#include "cmemory.h"
#include "sprpimpl.h"

/* it is official IDNA ACE Prefix is "xn--" */
static const UChar ACE_PREFIX[] ={ 0x0078,0x006E,0x002d,0x002d } ;
#define ACE_PREFIX_LENGTH 4

#define MAX_LABEL_LENGTH 63
#define HYPHEN      0x002D
/* The Max length of the labels should not be more than 64 */
#define MAX_LABEL_BUFFER_SIZE 100 
#define MAX_IDN_BUFFER_SIZE   300

#define CAPITAL_A        0x0041
#define CAPITAL_Z        0x005A
#define LOWER_CASE_DELTA 0x0020
#define FULL_STOP        0x002E

inline static UChar 
toASCIILower(UChar ch){
    if(CAPITAL_A <= ch && ch <= CAPITAL_Z){
        return ch + LOWER_CASE_DELTA;
    }
    return ch;
}

inline static UBool 
startsWithPrefix(const UChar* src , int32_t srcLength){
    UBool startsWithPrefix = TRUE;

    if(srcLength < ACE_PREFIX_LENGTH){
        return FALSE;
    }

    for(int8_t i=0; i< ACE_PREFIX_LENGTH; i++){
        if(toASCIILower(src[i]) != ACE_PREFIX[i]){
            startsWithPrefix = FALSE;
        }
    }
    return startsWithPrefix;
}

inline static void 
toASCIILower(UChar* src, int32_t srcLen){
    for(int32_t i=0; i<srcLen; i++){
        src[i] = toASCIILower(src[i]);
    }
}

inline static int32_t
compareCaseInsensitiveASCII(const UChar* s1, int32_t s1Len, 
                            const UChar* s2, int32_t s2Len){
    
    int32_t minLength;
    int32_t lengthResult;

    // are we comparing different lengths?
    if(s1Len != s2Len) {
        if(s1Len < s2Len) {
            minLength = s1Len;
            lengthResult = -1;
        } else {
            minLength = s2Len;
            lengthResult = 1;
        }
    } else {
        // ok the lengths are equal
        minLength = s1Len;
        lengthResult = 0;
    }

    UChar c1,c2;
    int32_t rc;

    for(int32_t i =0;/* no condition */;i++) {

        /* If we reach the ends of both strings then they match */
        if(i == minLength) {
            return lengthResult;
        }
        
        c1 = s1[i];
        c2 = s2[i];
        
        /* Case-insensitive comparison */
        if(c1!=c2) {
            rc=(int32_t)toASCIILower(c1)-(int32_t)toASCIILower(c2);
            if(rc!=0) {
                lengthResult=rc;
                break;
            }
        }
    }
    return lengthResult;
}


U_CAPI int32_t U_EXPORT2
uidna_toASCII(const UChar* src, int32_t srcLength, 
              UChar* dest, int32_t destCapacity,
              int32_t options,
              UParseError* parseError,
              UErrorCode* status){
    
    if(status == NULL || U_FAILURE(*status)){
        return 0;
    }
    if((srcLength < -1) || (destCapacity<0) || (!dest && destCapacity > 0)){
        *status = U_ILLEGAL_ARGUMENT_ERROR;
        return 0;
    }
    UChar b1Stack[MAX_LABEL_BUFFER_SIZE], b2Stack[MAX_LABEL_BUFFER_SIZE];
    //initialize pointers to stack buffers
    UChar  *b1 = b1Stack, *b2 = b2Stack;
    int32_t b1Len, b2Len, 
            b1Capacity = MAX_LABEL_BUFFER_SIZE, 
            b2Capacity = MAX_LABEL_BUFFER_SIZE ,
            reqLength=0;


    UBool* caseFlags = NULL;
    
    // the source contains all ascii codepoints
    UBool srcIsASCII  = TRUE;
    // assume the source contains all LDH codepoints
    UBool srcIsLDH = TRUE; 

    int32_t j=0;

    //get the options
    UBool allowUnassigned   = (UBool)((options & UIDNA_ALLOW_UNASSIGNED) != 0);
    UBool useSTD3ASCIIRules = (UBool)((options & UIDNA_USE_STD3_RULES) != 0);
    
    int32_t failPos = -1;
    // step 2
    StringPrep* prep = StringPrep::createNameprepInstance(*status);

    if(U_FAILURE(*status)){
        goto CLEANUP;
    }
    
    b1Len = prep->process(src,srcLength,b1, b1Capacity,allowUnassigned, parseError, *status);
    
    if(*status == U_BUFFER_OVERFLOW_ERROR){
        // redo processing of string
        // we do not have enough room so grow the buffer
        b1 = (UChar*) uprv_malloc(b1Len * U_SIZEOF_UCHAR);
        if(b1==NULL){
            *status = U_MEMORY_ALLOCATION_ERROR;
            goto CLEANUP;
        }

        *status = U_ZERO_ERROR; // reset error
        
        b1Len = prep->process(src,srcLength,b1, b1Len,allowUnassigned, parseError, *status);
    }
    // error bail out
    if(U_FAILURE(*status)){
        goto CLEANUP;
    }

    // step 3 & 4
    for( j=0;j<b1Len;j++){
        if(b1[j] > 0x7F){
            srcIsASCII = FALSE;
        }
        // here we do not assemble surrogates
        // since we know that LDH code points
        // are in the ASCII range only
        if(prep->isLDHChar(b1[j])==FALSE){
            srcIsLDH = FALSE;
            failPos = j;
        }
    }
    
    if(useSTD3ASCIIRules == TRUE){
        // verify 3a and 3b
        if( srcIsLDH == FALSE /* source contains some non-LDH characters */
            || b1[0] ==  HYPHEN || b1[b1Len-1] == HYPHEN){
            *status = U_IDNA_STD3_ASCII_RULES_ERROR;

            /* populate the parseError struct */
            if(srcIsLDH==FALSE){
                // failPos is always set the index of failure
                syntaxError(b1,failPos, b1Len,parseError);
            }else if(b1[0] == HYPHEN){
                // fail position is 0 
                syntaxError(b1,0,b1Len,parseError);
            }else{
                // the last index in the source is always length-1
                syntaxError(b1, (b1Len>0) ? b1Len-1 : b1Len, b1Len,parseError);
            }

            goto CLEANUP;
        }
    }
    if(srcIsASCII){
        if(b1Len <= destCapacity){
            uprv_memmove(dest, b1, b1Len * U_SIZEOF_UCHAR);
            reqLength = b1Len;
        }else{
            reqLength = b1Len;
            goto CLEANUP;
        }
    }else{
        // step 5 : verify the sequence does not begin with ACE prefix
        if(!startsWithPrefix(b1,b1Len)){

            //step 6: encode the sequence with punycode

            // do not preserve the case flags for now!
            // TODO: Preserve the case while implementing the RFE
            // caseFlags = (UBool*) uprv_malloc(b1Len * sizeof(UBool));
            // uprv_memset(caseFlags,TRUE,b1Len);

            b2Len = u_strToPunycode(b1,b1Len,b2,b2Capacity,caseFlags, status);

            if(*status == U_BUFFER_OVERFLOW_ERROR){
                // redo processing of string
                /* we do not have enough room so grow the buffer*/
                b2 = (UChar*) uprv_malloc(b2Len * U_SIZEOF_UCHAR); 
                if(b2 == NULL){
                    *status = U_MEMORY_ALLOCATION_ERROR;
                    goto CLEANUP;
                }

                *status = U_ZERO_ERROR; // reset error
                
                b2Len = u_strToPunycode(b1,b1Len,b2,b2Len,caseFlags, status);
            }
            //error bail out
            if(U_FAILURE(*status)){
                goto CLEANUP;
            }
            // TODO : Reconsider while implementing the case preserve RFE
            // convert all codepoints to lower case ASCII
            // toASCIILower(b2,b2Len);
            reqLength = b2Len+ACE_PREFIX_LENGTH;

            if(reqLength > destCapacity){
                *status = U_BUFFER_OVERFLOW_ERROR;
                goto CLEANUP;
            }
            //Step 7: prepend the ACE prefix
            uprv_memcpy(dest,ACE_PREFIX,ACE_PREFIX_LENGTH * U_SIZEOF_UCHAR);
            //Step 6: copy the contents in b2 into dest
            uprv_memcpy(dest+ACE_PREFIX_LENGTH, b2, b2Len * U_SIZEOF_UCHAR);

        }else{
            *status = U_IDNA_ACE_PREFIX_ERROR; 
            //position of failure is 0
            syntaxError(b1,0,b1Len,parseError);
            goto CLEANUP;
        }
    }

    if(reqLength > MAX_LABEL_LENGTH){
        *status = U_IDNA_LABEL_TOO_LONG_ERROR;
    }

CLEANUP:
    if(b1 != b1Stack){
        uprv_free(b1);
    }
    if(b2 != b2Stack){
        uprv_free(b2);
    }
    uprv_free(caseFlags);
    
    delete prep;

    return u_terminateUChars(dest, destCapacity, reqLength, status);
}


U_CAPI int32_t U_EXPORT2
uidna_toUnicode(const UChar* src, int32_t srcLength,
                UChar* dest, int32_t destCapacity,
                int32_t options,
                UParseError* parseError,
                UErrorCode* status){

    if(status == NULL || U_FAILURE(*status)){
        return 0;
    }
    if((srcLength < -1) || (destCapacity<0) || (!dest && destCapacity > 0)){
        *status = U_ILLEGAL_ARGUMENT_ERROR;
        return 0;
    }

    //get the options
    UBool allowUnassigned   = (UBool)((options & UIDNA_ALLOW_UNASSIGNED) != 0);
   // UBool useSTD3ASCIIRules = (UBool)((options & UIDNA_USE_STD3_RULES) != 0);
    
    UChar b1Stack[MAX_LABEL_BUFFER_SIZE], b2Stack[MAX_LABEL_BUFFER_SIZE], b3Stack[MAX_LABEL_BUFFER_SIZE];

    //initialize pointers to stack buffers
    UChar  *b1 = b1Stack, *b2 = b2Stack, *b1Prime=NULL, *b3=b3Stack;
    int32_t b1Len, b2Len, b1PrimeLen, b3Len,
            b1Capacity = MAX_LABEL_BUFFER_SIZE, 
            b2Capacity = MAX_LABEL_BUFFER_SIZE,
            b3Capacity = MAX_LABEL_BUFFER_SIZE,
            reqLength=0;
    
    StringPrep* prep = StringPrep::createNameprepInstance(*status);
    b1Len = 0;
    UBool* caseFlags = NULL;

    UBool srcIsASCII = TRUE;

    if(U_FAILURE(*status)){
        goto CLEANUP;
    }
    // step 1: find out if all the codepoints in src are ASCII  
    if(srcLength==-1){
        srcLength = 0;
        for(;src[srcLength]!=0;){
            if(src[srcLength]> 0x7f){
                srcIsASCII = FALSE;
            }
            srcLength++;
        }
    }else{
        for(int32_t j=0; j<srcLength; j++){
            if(src[j]> 0x7f){
                srcIsASCII = FALSE;
            }
        }
    }

    if(srcIsASCII == FALSE){
        // step 2: process the string
        b1Len = prep->process(src,srcLength,b1,b1Capacity,allowUnassigned, parseError, *status);
        if(*status == U_BUFFER_OVERFLOW_ERROR){
            // redo processing of string
            /* we do not have enough room so grow the buffer*/
            b1 = (UChar*) uprv_malloc(b1Len * U_SIZEOF_UCHAR);
            if(b1==NULL){
                *status = U_MEMORY_ALLOCATION_ERROR;
                goto CLEANUP;
            }

            *status = U_ZERO_ERROR; // reset error
            
            b1Len = prep->process(src,srcLength,b1, b1Len,allowUnassigned,  parseError, *status);
        }
        //bail out on error
        if(U_FAILURE(*status)){
            goto CLEANUP;
        }
    }else{

        //just point src to b1
        b1 = (UChar*) src;
        b1Len = srcLength;
    }
    //step 3: verify ACE Prefix
    if(startsWithPrefix(src,srcLength)){

        //step 4: Remove the ACE Prefix
        b1Prime = b1 + ACE_PREFIX_LENGTH;
        b1PrimeLen  = b1Len - ACE_PREFIX_LENGTH;

        //step 5: Decode using punycode
        b2Len = u_strFromPunycode(b1Prime, b1PrimeLen, b2, b2Capacity, caseFlags,status);
        
        if(*status == U_BUFFER_OVERFLOW_ERROR){
            // redo processing of string
            /* we do not have enough room so grow the buffer*/
            b2 = (UChar*) uprv_malloc(b2Len * U_SIZEOF_UCHAR);
            if(b2==NULL){
                *status = U_MEMORY_ALLOCATION_ERROR;
                goto CLEANUP;
            }

            *status = U_ZERO_ERROR; // reset error
            
            b2Len =  u_strFromPunycode(b1Prime, b1PrimeLen, b2, b2Len, caseFlags, status);
            
        }
        
        
        //step 6:Apply toASCII
        b3Len = uidna_toASCII(b2, b2Len, b3, b3Capacity,options,parseError, status);

        if(*status == U_BUFFER_OVERFLOW_ERROR){
            // redo processing of string
            /* we do not have enough room so grow the buffer*/
            b3 = (UChar*) uprv_malloc(b3Len * U_SIZEOF_UCHAR);
            if(b3==NULL){
                *status = U_MEMORY_ALLOCATION_ERROR;
                goto CLEANUP;
            }

            *status = U_ZERO_ERROR; // reset error
            
            b3Len =  uidna_toASCII(b2,b2Len,b3,b3Len,options,parseError, status);
        
        }
        //bail out on error
        if(U_FAILURE(*status)){
            goto CLEANUP;
        }

        //step 7: verify
        if(compareCaseInsensitiveASCII(b1, b1Len, b3, b3Len) !=0){
            *status = U_IDNA_VERIFICATION_ERROR; 
            goto CLEANUP;
        }

        //step 8: return output of step 5
        reqLength = b2Len;
        if(b2Len <= destCapacity) {
            uprv_memmove(dest, b2, b2Len * U_SIZEOF_UCHAR);
        }
    }else{
        //copy the source to destination
        if(srcLength <= destCapacity){
            uprv_memmove(dest,src,srcLength * U_SIZEOF_UCHAR);
        }
        reqLength = srcLength;
    }

CLEANUP:

    if(b1 != b1Stack && b1!=src){
        uprv_free(b1);
    }
    if(b2 != b2Stack){
        uprv_free(b2);
    }
    uprv_free(caseFlags);
    
    delete prep;

    return u_terminateUChars(dest, destCapacity, reqLength, status);
}

// returns the length of the label excluding the separator
// if *limit == separator then the length returned does not include 
// the separtor.
static int32_t
getNextSeparator(UChar *src,int32_t srcLength,StringPrep* prep,
                 UChar **limit,
                 UBool *done,
                 UErrorCode *status){
    if(srcLength == -1){
        int32_t i;
        for(i=0 ; ;i++){
            if(src[i] == 0){
                *limit = src + i; // point to null
                *done = TRUE;
                return i;
            }
            if(prep->isLabelSeparator(src[i],*status)){
                *limit = src + (i+1); // go past the delimiter
                return i;
                
            }
        }
    }else{
        int32_t i;
        for(i=0;i<srcLength;i++){
            if(prep->isLabelSeparator(src[i],*status)){
                *limit = src + (i+1); // go past the delimiter
                return i;
            }
        }
        // we have not found the delimiter
        // if(i==srcLength)
        *limit = src+srcLength;
        *done = TRUE;

        return i;
    }
}

U_CAPI int32_t U_EXPORT2
uidna_IDNToASCII(  const UChar *src, int32_t srcLength,
                   UChar* dest, int32_t destCapacity,
                   int32_t options,
                   UParseError *parseError,
                   UErrorCode *status){

    if(status == NULL || U_FAILURE(*status)){
        return 0;
    }
    if((srcLength < -1) || (destCapacity<0) || (!dest && destCapacity > 0)){
        *status = U_ILLEGAL_ARGUMENT_ERROR;
        return 0;
    }

    int32_t reqLength = 0;

    StringPrep* prep = StringPrep::createNameprepInstance(*status);
    
    if(U_FAILURE(*status)){
        return 0;
    }

    //initialize pointers 
    UChar *delimiter = (UChar*)src;
    UChar *labelStart = (UChar*)src;
    UChar *currentDest = (UChar*) dest;
    int32_t remainingLen = srcLength;
    int32_t remainingDestCapacity = destCapacity;
    int32_t labelLen = 0, labelReqLength = 0;
    UBool done = FALSE;


    for(;;){

        labelLen = getNextSeparator(labelStart,remainingLen, prep, &delimiter,&done, status);
        
        labelReqLength = uidna_toASCII( labelStart, labelLen, 
                                        currentDest, remainingDestCapacity, 
                                        options, parseError, status);

        if(*status == U_BUFFER_OVERFLOW_ERROR){
            
            *status = U_ZERO_ERROR; // reset error
            remainingDestCapacity = 0;
        }

    
        if(U_FAILURE(*status)){
            break;
        }
        
        reqLength +=labelReqLength;
        // adjust the destination pointer
        if(labelReqLength < remainingDestCapacity){
            currentDest = currentDest + labelReqLength;
            remainingDestCapacity -= labelReqLength;
        }else{
            // should never occur
            remainingDestCapacity = 0;
        }
        if(done == TRUE){
            break;
        }

        // add the label separator
        if(remainingDestCapacity > 0){
            *currentDest++ = FULL_STOP;
            remainingDestCapacity--;
        }
        reqLength++;           

        labelStart = delimiter;
        if(remainingLen >0 ){
            remainingLen = srcLength - (delimiter - src);
        }

    }
   
    delete prep;
    
    return u_terminateUChars(dest, destCapacity, reqLength, status);
}

U_CAPI int32_t U_EXPORT2
uidna_IDNToUnicode(  const UChar* src, int32_t srcLength,
                     UChar* dest, int32_t destCapacity,
                     int32_t options,
                     UParseError* parseError,
                     UErrorCode* status){
    
    if(status == NULL || U_FAILURE(*status)){
        return 0;
    }
    if((srcLength < -1) || (destCapacity<0) || (!dest && destCapacity > 0)){
        *status = U_ILLEGAL_ARGUMENT_ERROR;
        return 0;
    }

    int32_t reqLength = 0;

    StringPrep* prep = StringPrep::createNameprepInstance(*status);
    
    if(U_FAILURE(*status)){
        return 0;
    }

    //initialize pointers
    UChar *delimiter = (UChar*)src;
    UChar *labelStart = (UChar*)src;
    UChar *currentDest = (UChar*) dest;
    int32_t remainingLen = srcLength;
    int32_t remainingDestCapacity = destCapacity;
    int32_t labelLen = 0, labelReqLength = 0;
    UBool done = FALSE;


    for(;;){

        labelLen = getNextSeparator(labelStart,remainingLen, prep, &delimiter,&done, status);
        
        labelReqLength = uidna_toUnicode(labelStart, labelLen, 
                                         currentDest, remainingDestCapacity, 
                                         options, parseError, status);

        if(*status == U_BUFFER_OVERFLOW_ERROR){
            
            *status = U_ZERO_ERROR; // reset error
            remainingDestCapacity = 0;
        }

    
        if(U_FAILURE(*status)){
            break;
        }
        
        reqLength +=labelReqLength;
        // adjust the destination pointer
        if(labelReqLength < remainingDestCapacity){
            currentDest = currentDest + labelReqLength;
            remainingDestCapacity -= labelReqLength;
        }else{
            // should never occur
            remainingDestCapacity = 0;
        }

        if(done == TRUE){
            break;
        }

        // add the label separator
        if(remainingDestCapacity > 0){
            *currentDest++ = FULL_STOP;
            remainingDestCapacity--;
        }
        reqLength++;           

        labelStart = delimiter;
        if(remainingLen >0 ){
            remainingLen = srcLength - (delimiter - src);
        }

    }
   
    delete prep;
    
    return u_terminateUChars(dest, destCapacity, reqLength, status);
}

U_CAPI int32_t U_EXPORT2
uidna_compare(  const UChar *s1, int32_t length1,
                const UChar *s2, int32_t length2,
                int32_t options,
                UErrorCode* status){

    if(status == NULL || U_FAILURE(*status)){
        return -1;
    }

    UChar b1Stack[MAX_IDN_BUFFER_SIZE], b2Stack[MAX_IDN_BUFFER_SIZE];
    UChar *b1 = b1Stack, *b2 = b2Stack;
    int32_t b1Len, b2Len, b1Capacity = MAX_IDN_BUFFER_SIZE, b2Capacity = MAX_IDN_BUFFER_SIZE;
    int32_t result=-1;
    
    UParseError parseError;

    b1Len = uidna_IDNToASCII(s1, length1, b1, b1Capacity, options, &parseError, status);
    if(*status == U_BUFFER_OVERFLOW_ERROR){
        // redo processing of string
        b1 = (UChar*) uprv_malloc(b1Len * U_SIZEOF_UCHAR);
        if(b1==NULL){
            *status = U_MEMORY_ALLOCATION_ERROR;
            goto CLEANUP;
        }

        *status = U_ZERO_ERROR; // reset error
        
        b1Len = uidna_IDNToASCII(s1,length1,b1,b1Len, options, &parseError, status);
        
    }

    b2Len = uidna_IDNToASCII(s2,length2, b2,b2Capacity, options, &parseError, status);
    if(*status == U_BUFFER_OVERFLOW_ERROR){
        // redo processing of string
        b2 = (UChar*) uprv_malloc(b2Len * U_SIZEOF_UCHAR);
        if(b2==NULL){
            *status = U_MEMORY_ALLOCATION_ERROR;
            goto CLEANUP;
        }

        *status = U_ZERO_ERROR; // reset error
        
        b2Len = uidna_IDNToASCII(s2, length2, b2, b2Len, options, &parseError, status);
        
    }
    // when toASCII is applied all label separators are replaced with FULL_STOP
    result = compareCaseInsensitiveASCII(b1,b1Len,b2,b2Len);

CLEANUP:
    if(b1 != b1Stack){
        uprv_free(b1);
    }

    if(b2 != b2Stack){
        uprv_free(b2);
    }

    return result;
}

#endif /* #if !UCONFIG_NO_IDNA */
