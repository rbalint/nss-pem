/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the Netscape security libraries.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1994-2000
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Rob Crittenden (rcritten@redhat.com)
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "ckpem.h"

/*
 * prsa.c
 *
 * This file implements the NSSCKMDMechnaism and NSSCKMDCryptoOperation objects
 * for the RSA operation.
 */

#include <nssckmdt.h>
#include <secdert.h>
#include <secoid.h>

#define SSL3_SHAMD5_HASH_SIZE  36       /* LEN_MD5 (16) + LEN_SHA1 (20) */

#ifdef HAVE_LOWKEYTI_H
#   include <lowkeyti.h>
#else
/*
 * we are dealing with older NSS that does not include the patch
 * for https://bugzilla.mozilla.org/show_bug.cgi?id=1263179 yet
 */
struct NSSLOWKEYAttributeStr {
    SECItem attrType;
    SECItem *attrValue;
};
typedef struct NSSLOWKEYAttributeStr NSSLOWKEYAttribute;
struct NSSLOWKEYPrivateKeyInfoStr {
    PLArenaPool *arena;
    SECItem version;
    SECAlgorithmID algorithm;
    SECItem privateKey;
    NSSLOWKEYAttribute **attributes;
};
typedef struct NSSLOWKEYPrivateKeyInfoStr NSSLOWKEYPrivateKeyInfo;
#endif

const SEC_ASN1Template pem_RSAPrivateKeyTemplate[] = {
    {SEC_ASN1_SEQUENCE, 0, NULL, sizeof(NSSLOWKEYPrivateKey)} ,
    {SEC_ASN1_INTEGER, offsetof(NSSLOWKEYPrivateKey, u.rsa.version)},
    {SEC_ASN1_INTEGER, offsetof(NSSLOWKEYPrivateKey, u.rsa.modulus)},
    {SEC_ASN1_INTEGER, offsetof(NSSLOWKEYPrivateKey, u.rsa.publicExponent)},
    {SEC_ASN1_INTEGER, offsetof(NSSLOWKEYPrivateKey, u.rsa.privateExponent)},
    {SEC_ASN1_INTEGER, offsetof(NSSLOWKEYPrivateKey, u.rsa.prime1)},
    {SEC_ASN1_INTEGER, offsetof(NSSLOWKEYPrivateKey, u.rsa.prime2)},
    {SEC_ASN1_INTEGER, offsetof(NSSLOWKEYPrivateKey, u.rsa.exponent1)},
    {SEC_ASN1_INTEGER, offsetof(NSSLOWKEYPrivateKey, u.rsa.exponent2)},
    {SEC_ASN1_INTEGER, offsetof(NSSLOWKEYPrivateKey, u.rsa.coefficient)},
    {0}
};

static const SEC_ASN1Template pem_AttributeTemplate[] = {
    { SEC_ASN1_SEQUENCE,
      0, NULL, sizeof(NSSLOWKEYAttribute) },
    { SEC_ASN1_OBJECT_ID, offsetof(NSSLOWKEYAttribute, attrType) },
    { SEC_ASN1_SET_OF | SEC_ASN1_XTRN, offsetof(NSSLOWKEYAttribute, attrValue),
      SEC_ASN1_SUB(SEC_AnyTemplate) },
    { 0 }
};

static const SEC_ASN1Template pem_SetOfAttributeTemplate[] = {
    { SEC_ASN1_SET_OF, 0, pem_AttributeTemplate },
};

const SEC_ASN1Template pem_PrivateKeyInfoTemplate[] = {
    { SEC_ASN1_SEQUENCE,
      0, NULL, sizeof(NSSLOWKEYPrivateKeyInfo) },
    { SEC_ASN1_INTEGER,
      offsetof(NSSLOWKEYPrivateKeyInfo,version) },
    { SEC_ASN1_INLINE | SEC_ASN1_XTRN,
      offsetof(NSSLOWKEYPrivateKeyInfo,algorithm),
      SEC_ASN1_SUB(SECOID_AlgorithmIDTemplate) },
    { SEC_ASN1_OCTET_STRING,
      offsetof(NSSLOWKEYPrivateKeyInfo,privateKey) },
    { SEC_ASN1_OPTIONAL | SEC_ASN1_CONSTRUCTED | SEC_ASN1_CONTEXT_SPECIFIC | 0,
      offsetof(NSSLOWKEYPrivateKeyInfo, attributes),
      pem_SetOfAttributeTemplate },
    { 0 }
};

/* Declarations */
SECStatus pem_RSA_Sign(NSSLOWKEYPrivateKey * key, unsigned char *output,
                       unsigned int *outputLen, unsigned int maxOutputLen,
                       unsigned char *input, unsigned int inputLen);
SECStatus pem_RSA_DecryptBlock(NSSLOWKEYPrivateKey * key,
                               unsigned char *output, unsigned int *outputLen,
                               unsigned int maxOutputLen, unsigned char *input,
                               unsigned int inputLen);

void prepare_low_rsa_priv_key_for_asn1(NSSLOWKEYPrivateKey * key)
{
    key->u.rsa.modulus.type = siUnsignedInteger;
    key->u.rsa.publicExponent.type = siUnsignedInteger;
    key->u.rsa.privateExponent.type = siUnsignedInteger;
    key->u.rsa.prime1.type = siUnsignedInteger;
    key->u.rsa.prime2.type = siUnsignedInteger;
    key->u.rsa.exponent1.type = siUnsignedInteger;
    key->u.rsa.exponent2.type = siUnsignedInteger;
    key->u.rsa.coefficient.type = siUnsignedInteger;
}

unsigned int
pem_PrivateModulusLen(NSSLOWKEYPrivateKey * privk)
{

    unsigned char b0;

    switch (privk->keyType) {
    case NSSLOWKEYRSAKey:
        b0 = privk->u.rsa.modulus.data[0];
        return b0 ? privk->u.rsa.modulus.len : privk->u.rsa.modulus.len -
            1;
    default:
        break;
    }
    return 0;
}

struct SFTKHashSignInfoStr {
    SECOidTag hashOid;
    NSSLOWKEYPrivateKey *key;
};
typedef struct SFTKHashSignInfoStr SFTKHashSignInfo;

void
pem_DestroyPrivateKey(NSSLOWKEYPrivateKey * privk)
{
    if (privk && privk->arena) {
        PORT_FreeArena(privk->arena, PR_TRUE);
    }
    NSS_ZFreeIf(privk);
}

/* decode and parse the rawkey into the lpk structure */
static NSSLOWKEYPrivateKey *
pem_getPrivateKey(PLArenaPool *arena, SECItem *rawkey, CK_RV * pError, NSSItem *modulus)
{
    NSSLOWKEYPrivateKey *lpk = NULL;
    SECStatus rv = SECFailure;
    NSSLOWKEYPrivateKeyInfo *pki = NULL;
    SECItem *keysrc = NULL;

    /* make sure SECOID is initialized - not sure why we have to do this outside of nss_Init */
    if (SECSuccess != (rv = SECOID_Init())) {
        *pError = CKR_GENERAL_ERROR;
        return NULL; /* wha???? */
    }

    pki = (NSSLOWKEYPrivateKeyInfo*)PORT_ArenaZAlloc(arena,
                                                     sizeof(NSSLOWKEYPrivateKeyInfo));
    if(!pki) {
        *pError = CKR_HOST_MEMORY;
        goto done;
    }

    /* let's first see if this is a "raw" RSA private key or an RSA private key in PKCS#8 format */
    rv = SEC_ASN1DecodeItem(arena, pki, pem_PrivateKeyInfoTemplate, rawkey);
    if (rv != SECSuccess) {
        /* not PKCS#8 - assume it's a "raw" RSA private key */
        plog("Failed to decode key, assuming raw RSA private key\n");
        keysrc = rawkey;
    } else if (SECOID_GetAlgorithmTag(&pki->algorithm) == SEC_OID_PKCS1_RSA_ENCRYPTION) {
        keysrc = &pki->privateKey;
    } else { /* unsupported */
        *pError = CKR_FUNCTION_NOT_SUPPORTED;
        goto done;
    }

    lpk = (NSSLOWKEYPrivateKey *) NSS_ZAlloc(NULL,
                                             sizeof(NSSLOWKEYPrivateKey));
    if (lpk == NULL) {
        *pError = CKR_HOST_MEMORY;
        goto done;
    }

    lpk->arena = arena;
    lpk->keyType = NSSLOWKEYRSAKey;
    prepare_low_rsa_priv_key_for_asn1(lpk);

    /* I don't know what this is supposed to accomplish.  We free the old
       modulus data and set it again, making a copy of the new data.
       But we just allocated a new empty key structure above with
       NSS_ZAlloc.  So lpk->u.rsa.modulus.data is NULL and
       lpk->u.rsa.modulus.len.  If the intention is to free the old
       modulus data, why not just set it to NULL after freeing?  Why
       go through this unnecessary and confusing copying code?
    */
    if (modulus) {
        NSS_ZFreeIf(modulus->data);
        modulus->data = (void *) NSS_ZAlloc(NULL, lpk->u.rsa.modulus.len);
        modulus->size = lpk->u.rsa.modulus.len;
        memcpy(modulus->data,
                lpk->u.rsa.modulus.data,
                lpk->u.rsa.modulus.len);
    }

    /* decode the private key and any algorithm parameters */
    rv = SEC_QuickDERDecodeItem(arena, lpk, pem_RSAPrivateKeyTemplate,
                                keysrc);

    if (rv != SECSuccess) {
        plog("SEC_QuickDERDecodeItem failed\n");
        *pError = CKR_KEY_TYPE_INCONSISTENT;
        /* do not use pem_DestroyPrivateKey() to avoid double free of arena */
        NSS_ZFreeIf(lpk);
        return NULL;
    }

done:
    return lpk;
}

CK_RV
pem_PopulateModulusExponent(pemInternalObject * io)
{
    CK_RV error = CKR_OK;
    const NSSItem *classItem;
    const NSSItem *keyType;
    NSSLOWKEYPrivateKey *lpk = NULL;
    PLArenaPool *arena;

    classItem = pem_FetchAttribute(io, CKA_CLASS, &error);
    if (error != CKR_OK)
        return error;

    keyType = pem_FetchAttribute(io, CKA_KEY_TYPE, &error);
    if (error != CKR_OK)
        return error;

    /* make sure we have the right objects */
    if (((const NSSItem *) NULL == classItem) ||
        (sizeof(CK_OBJECT_CLASS) != classItem->size) ||
        (CKO_PRIVATE_KEY != *(CK_OBJECT_CLASS *) classItem->data) ||
        ((const NSSItem *) NULL == keyType) ||
        (sizeof(CK_KEY_TYPE) != keyType->size) ||
        (CKK_RSA != *(CK_KEY_TYPE *) keyType->data)) {
        return CKR_KEY_TYPE_INCONSISTENT;
    }

    arena = PORT_NewArena(2048);
    if (!arena) {
        return CKR_HOST_MEMORY;
    }

    lpk = pem_getPrivateKey(arena, io->u.key.key.privateKey, &error, NULL);
    if (lpk == NULL) {
        plog("pem_PopulateModulusExponent: pem_getPrivateKey returned NULL, error 0x%08x\n", error);
        PORT_FreeArena(arena, PR_FALSE);
        return (error ? error : CKR_KEY_TYPE_INCONSISTENT);
    }

    NSS_ZFreeIf(io->u.key.key.modulus.data);
    io->u.key.key.modulus.data =
        (void *) NSS_ZAlloc(NULL, lpk->u.rsa.modulus.len);
    io->u.key.key.modulus.size = lpk->u.rsa.modulus.len;
    memcpy(io->u.key.key.modulus.data,
            lpk->u.rsa.modulus.data,
            lpk->u.rsa.modulus.len);

    NSS_ZFreeIf(io->u.key.key.exponent.data);
    io->u.key.key.exponent.data =
        (void *) NSS_ZAlloc(NULL, lpk->u.rsa.publicExponent.len);
    io->u.key.key.exponent.size = lpk->u.rsa.publicExponent.len;
    memcpy(io->u.key.key.exponent.data,
            lpk->u.rsa.publicExponent.data,
            lpk->u.rsa.publicExponent.len);

    NSS_ZFreeIf(io->u.key.key.privateExponent.data);
    io->u.key.key.privateExponent.data =
        (void *) NSS_ZAlloc(NULL, lpk->u.rsa.privateExponent.len);
    io->u.key.key.privateExponent.size = lpk->u.rsa.privateExponent.len;
    memcpy(io->u.key.key.privateExponent.data,
            lpk->u.rsa.privateExponent.data,
            lpk->u.rsa.privateExponent.len);

    NSS_ZFreeIf(io->u.key.key.prime1.data);
    io->u.key.key.prime1.data =
        (void *) NSS_ZAlloc(NULL, lpk->u.rsa.prime1.len);
    io->u.key.key.prime1.size = lpk->u.rsa.prime1.len;
    memcpy(io->u.key.key.prime1.data,
            lpk->u.rsa.prime1.data,
            lpk->u.rsa.prime1.len);

    NSS_ZFreeIf(io->u.key.key.prime2.data);
    io->u.key.key.prime2.data =
        (void *) NSS_ZAlloc(NULL, lpk->u.rsa.prime2.len);
    io->u.key.key.prime2.size = lpk->u.rsa.prime2.len;
    memcpy(io->u.key.key.prime2.data,
            lpk->u.rsa.prime2.data,
            lpk->u.rsa.prime2.len);

    NSS_ZFreeIf(io->u.key.key.exponent1.data);
    io->u.key.key.exponent1.data =
        (void *) NSS_ZAlloc(NULL, lpk->u.rsa.exponent1.len);
    io->u.key.key.exponent1.size = lpk->u.rsa.exponent1.len;
    memcpy(io->u.key.key.exponent1.data,
            lpk->u.rsa.exponent1.data,
            lpk->u.rsa.exponent1.len);

    NSS_ZFreeIf(io->u.key.key.exponent2.data);
    io->u.key.key.exponent2.data =
        (void *) NSS_ZAlloc(NULL, lpk->u.rsa.exponent2.len);
    io->u.key.key.exponent2.size = lpk->u.rsa.exponent2.len;
    memcpy(io->u.key.key.exponent2.data,
            lpk->u.rsa.exponent2.data,
            lpk->u.rsa.exponent2.len);

    NSS_ZFreeIf(io->u.key.key.coefficient.data);
    io->u.key.key.coefficient.data =
        (void *) NSS_ZAlloc(NULL, lpk->u.rsa.coefficient.len);
    io->u.key.key.coefficient.size = lpk->u.rsa.coefficient.len;
    memcpy(io->u.key.key.coefficient.data,
            lpk->u.rsa.coefficient.data,
            lpk->u.rsa.coefficient.len);

    pem_DestroyPrivateKey(lpk);
    return CKR_OK;
}

typedef struct pemInternalCryptoOperationRSAPrivStr
               pemInternalCryptoOperationRSAPriv;
struct pemInternalCryptoOperationRSAPrivStr
{
    NSSCKMDCryptoOperation mdOperation;
    NSSCKMDMechanism *mdMechanism;
    pemInternalObject *iKey;
    NSSLOWKEYPrivateKey *lpk;
    NSSItem buffer;
};

/*
 * pem_mdCryptoOperationRSAPriv_Create
 */
static NSSCKMDCryptoOperation *
pem_mdCryptoOperationRSAPriv_Create
(
    const NSSCKMDCryptoOperation * proto,
    NSSCKMDMechanism * mdMechanism,
    NSSCKMDObject * mdKey,
    CK_RV * pError
)
{
    pemInternalObject *iKey = (pemInternalObject *) mdKey->etc;
    const NSSItem *classItem;
    const NSSItem *keyType;
    pemInternalCryptoOperationRSAPriv *iOperation;
    NSSLOWKEYPrivateKey *lpk = NULL;
    PLArenaPool *arena;

    classItem = pem_FetchAttribute(iKey, CKA_CLASS, pError);
    if (*pError != CKR_OK)
        return (NSSCKMDCryptoOperation *) NULL;

    keyType = pem_FetchAttribute(iKey, CKA_KEY_TYPE, pError);
    if (*pError != CKR_OK)
        return (NSSCKMDCryptoOperation *) NULL;

    /* make sure we have the right objects */
    if (((const NSSItem *) NULL == classItem) ||
        (sizeof(CK_OBJECT_CLASS) != classItem->size) ||
        (CKO_PRIVATE_KEY != *(CK_OBJECT_CLASS *) classItem->data) ||
        ((const NSSItem *) NULL == keyType) ||
        (sizeof(CK_KEY_TYPE) != keyType->size) ||
        (CKK_RSA != *(CK_KEY_TYPE *) keyType->data)) {
        *pError = CKR_KEY_TYPE_INCONSISTENT;
        return (NSSCKMDCryptoOperation *) NULL;
    }

    arena =  PORT_NewArena(2048);
    if (!arena) {
        *pError = CKR_HOST_MEMORY;
        return (NSSCKMDCryptoOperation *) NULL;
    }

    lpk = pem_getPrivateKey(arena, iKey->u.key.key.privateKey, pError, &iKey->u.key.key.modulus);
    if (lpk == NULL) {
        plog("pem_mdCryptoOperationRSAPriv_Create: pem_getPrivateKey returned NULL, pError 0x%08x\n", *pError);
        PORT_FreeArena(arena, PR_FALSE);
        return (NSSCKMDCryptoOperation *) NULL;
    }

    iOperation = NSS_ZNEW(NULL, pemInternalCryptoOperationRSAPriv);
    if ((pemInternalCryptoOperationRSAPriv *) NULL == iOperation) {
        *pError = CKR_HOST_MEMORY;
        return (NSSCKMDCryptoOperation *) NULL;
    }
    iOperation->mdMechanism = mdMechanism;
    iOperation->iKey = iKey;
    iOperation->lpk = lpk;

    memcpy(&iOperation->mdOperation, proto, sizeof iOperation->mdOperation);
    iOperation->mdOperation.etc = iOperation;

    return &iOperation->mdOperation;
}

static void
pem_mdCryptoOperationRSAPriv_Destroy
(
    NSSCKMDCryptoOperation * mdOperation,
    NSSCKFWCryptoOperation * fwOperation,
    NSSCKMDInstance * mdInstance,
    NSSCKFWInstance * fwInstance
)
{
    pemInternalCryptoOperationRSAPriv *iOperation =
        (pemInternalCryptoOperationRSAPriv *) mdOperation->etc;

    NSS_ZFreeIf(iOperation->buffer.data);
    iOperation->buffer.data = NULL;

    pem_DestroyPrivateKey(iOperation->lpk);
    iOperation->lpk = NULL;
    NSS_ZFreeIf(iOperation);
}

static CK_ULONG
pem_mdCryptoOperationRSA_GetFinalLength
(
    NSSCKMDCryptoOperation * mdOperation,
    NSSCKFWCryptoOperation * fwOperation,
    NSSCKMDSession * mdSession,
    NSSCKFWSession * fwSession,
    NSSCKMDToken * mdToken,
    NSSCKFWToken * fwToken,
    NSSCKMDInstance * mdInstance,
    NSSCKFWInstance * fwInstance,
    CK_RV * pError
)
{
    pemInternalCryptoOperationRSAPriv *iOperation =
        (pemInternalCryptoOperationRSAPriv *) mdOperation->etc;
    const NSSItem *modulus =
        pem_FetchAttribute(iOperation->iKey, CKA_MODULUS, pError);

    if (NULL == modulus) {
        *pError = CKR_FUNCTION_FAILED;
        return 0;
    }

    return modulus->size;
}


/*
 * pem_mdCryptoOperationRSADecrypt_GetOperationLength
 * we won't know the length until we actually decrypt the
 * input block. Since we go to all the work to decrypt the
 * the block, we'll save if for when the block is asked for
 */
static CK_ULONG
pem_mdCryptoOperationRSADecrypt_GetOperationLength
(
    NSSCKMDCryptoOperation * mdOperation,
    NSSCKFWCryptoOperation * fwOperation,
    NSSCKMDSession * mdSession,
    NSSCKFWSession * fwSession,
    NSSCKMDToken * mdToken,
    NSSCKFWToken * fwToken,
    NSSCKMDInstance * mdInstance,
    NSSCKFWInstance * fwInstance,
    const NSSItem * input,
    CK_RV * pError
)
{
    pemInternalCryptoOperationRSAPriv *iOperation =
        (pemInternalCryptoOperationRSAPriv *) mdOperation->etc;
    SECStatus rv;

    /* FIXME: Just because Microsoft is broken doesn't mean I have to be.
     * but this is faster to do for now */

    /* Microsoft's Decrypt operation works in place. Since we don't want
     * to trash our input buffer, we make a copy of it */
    iOperation->buffer.data = NSS_ZAlloc(NULL, input->size);
    if (NULL == iOperation->buffer.data) {
        *pError = CKR_HOST_MEMORY;
        return 0;
    }
    /* copy data and initialize size of the NSSItem */
    memcpy(iOperation->buffer.data, input->data, input->size);
    iOperation->buffer.size = input->size;

    rv = pem_RSA_DecryptBlock(iOperation->lpk, iOperation->buffer.data,
                              &iOperation->buffer.size,
                              iOperation->buffer.size, input->data,
                              input->size);

    if (rv != SECSuccess) {
        return 0;
    }

    return iOperation->buffer.size;
}

/*
 * pem_mdCryptoOperationRSADecrypt_UpdateFinal
 *
 * NOTE: pem_mdCryptoOperationRSADecrypt_GetOperationLength is presumed to
 * have been called previously.
 */
static CK_RV
pem_mdCryptoOperationRSADecrypt_UpdateFinal
(
    NSSCKMDCryptoOperation * mdOperation,
    NSSCKFWCryptoOperation * fwOperation,
    NSSCKMDSession * mdSession,
    NSSCKFWSession * fwSession,
    NSSCKMDToken * mdToken,
    NSSCKFWToken * fwToken,
    NSSCKMDInstance * mdInstance,
    NSSCKFWInstance * fwInstance,
    const NSSItem * input,
    NSSItem * output
)
{
    pemInternalCryptoOperationRSAPriv *iOperation =
        (pemInternalCryptoOperationRSAPriv *) mdOperation->etc;
    NSSItem *buffer = &iOperation->buffer;

    if (NULL == buffer->data) {
        return CKR_GENERAL_ERROR;
    }
    memcpy(output->data, buffer->data, buffer->size);
    output->size = buffer->size;
    return CKR_OK;
}

/*
 * pem_mdCryptoOperationRSASign_UpdateFinal
 *
 */
static CK_RV
pem_mdCryptoOperationRSASign_UpdateFinal
(
    NSSCKMDCryptoOperation * mdOperation,
    NSSCKFWCryptoOperation * fwOperation,
    NSSCKMDSession * mdSession,
    NSSCKFWSession * fwSession,
    NSSCKMDToken * mdToken,
    NSSCKFWToken * fwToken,
    NSSCKMDInstance * mdInstance,
    NSSCKFWInstance * fwInstance,
    const NSSItem * input,
    NSSItem * output
)
{
    pemInternalCryptoOperationRSAPriv *iOperation =
        (pemInternalCryptoOperationRSAPriv *) mdOperation->etc;
    CK_RV error = CKR_OK;
    SECStatus rv = SECSuccess;

    rv = pem_RSA_Sign(iOperation->lpk, output->data, &output->size,
                      output->size, input->data, input->size);

    if (rv != SECSuccess) {
        error = CKR_GENERAL_ERROR;
    }

    return error;
}

NSS_IMPLEMENT_DATA const NSSCKMDCryptoOperation
pem_mdCryptoOperationRSADecrypt_proto = {
    NULL, /* etc */
    pem_mdCryptoOperationRSAPriv_Destroy,
    NULL, /* GetFinalLengh - not needed for one shot Decrypt/Encrypt */
    pem_mdCryptoOperationRSADecrypt_GetOperationLength,
    NULL, /* Final - not needed for one shot operation */
    NULL, /* Update - not needed for one shot operation */
    NULL, /* DigestUpdate - not needed for one shot operation */
    pem_mdCryptoOperationRSADecrypt_UpdateFinal,
    NULL, /* UpdateCombo - not needed for one shot operation */
    NULL, /* DigestKey - not needed for one shot operation */
    (void *) NULL /* null terminator */
};

NSS_IMPLEMENT_DATA const NSSCKMDCryptoOperation
pem_mdCryptoOperationRSASign_proto = {
    NULL, /* etc */
    pem_mdCryptoOperationRSAPriv_Destroy,
    pem_mdCryptoOperationRSA_GetFinalLength,
    NULL, /* GetOperationLengh - not needed for one shot Sign/Verify */
    NULL, /* Final - not needed for one shot operation */
    NULL, /* Update - not needed for one shot operation */
    NULL, /* DigestUpdate - not needed for one shot operation */
    pem_mdCryptoOperationRSASign_UpdateFinal,
    NULL, /* UpdateCombo - not needed for one shot operation */
    NULL, /* DigestKey - not needed for one shot operation */
    (void *) NULL /* null terminator */
};

/********** NSSCKMDMechansim functions ***********************/
/*
 * pem_mdMechanismRSA_Destroy
 */
static void
pem_mdMechanismRSA_Destroy
(
    NSSCKMDMechanism * mdMechanism,
    NSSCKFWMechanism * fwMechanism,
    NSSCKMDInstance * mdInstance,
    NSSCKFWInstance * fwInstance
)
{
    NSS_ZFreeIf(fwMechanism);
}

/*
 * pem_mdMechanismRSA_GetMinKeySize
 */
static CK_ULONG
pem_mdMechanismRSA_GetMinKeySize
(
    NSSCKMDMechanism * mdMechanism,
    NSSCKFWMechanism * fwMechanism,
    NSSCKMDToken * mdToken,
    NSSCKFWToken * fwToken,
    NSSCKMDInstance * mdInstance,
    NSSCKFWInstance * fwInstance,
    CK_RV * pError
)
{
    return 384;
}

/*
 * pem_mdMechanismRSA_GetMaxKeySize
 */
static CK_ULONG
pem_mdMechanismRSA_GetMaxKeySize
(
    NSSCKMDMechanism * mdMechanism,
    NSSCKFWMechanism * fwMechanism,
    NSSCKMDToken * mdToken,
    NSSCKFWToken * fwToken,
    NSSCKMDInstance * mdInstance,
    NSSCKFWInstance * fwInstance,
    CK_RV * pError
)
{
    return 16384;
}

/*
 * pem_mdMechanismRSA_DecryptInit
 */
static NSSCKMDCryptoOperation *
pem_mdMechanismRSA_DecryptInit
(
    NSSCKMDMechanism * mdMechanism,
    NSSCKFWMechanism * fwMechanism,
    CK_MECHANISM * pMechanism,
    NSSCKMDSession * mdSession,
    NSSCKFWSession * fwSession,
    NSSCKMDToken * mdToken,
    NSSCKFWToken * fwToken,
    NSSCKMDInstance * mdInstance,
    NSSCKFWInstance * fwInstance,
    NSSCKMDObject * mdKey,
    NSSCKFWObject * fwKey,
    CK_RV * pError
)
{
    return pem_mdCryptoOperationRSAPriv_Create
        (&pem_mdCryptoOperationRSADecrypt_proto, mdMechanism, mdKey,
         pError);
}

/*
 * pem_mdMechanismRSA_SignInit
 */
static NSSCKMDCryptoOperation *
pem_mdMechanismRSA_SignInit
(
    NSSCKMDMechanism * mdMechanism,
    NSSCKFWMechanism * fwMechanism,
    CK_MECHANISM * pMechanism,
    NSSCKMDSession * mdSession,
    NSSCKFWSession * fwSession,
    NSSCKMDToken * mdToken,
    NSSCKFWToken * fwToken,
    NSSCKMDInstance * mdInstance,
    NSSCKFWInstance * fwInstance,
    NSSCKMDObject * mdKey,
    NSSCKFWObject * fwKey,
    CK_RV * pError
)
{
    return pem_mdCryptoOperationRSAPriv_Create
        (&pem_mdCryptoOperationRSASign_proto, mdMechanism, mdKey, pError);
}

NSS_IMPLEMENT_DATA const NSSCKMDMechanism
pem_mdMechanismRSA = {
    (void *) NULL, /* etc */
    pem_mdMechanismRSA_Destroy,
    pem_mdMechanismRSA_GetMinKeySize,
    pem_mdMechanismRSA_GetMaxKeySize,
    NULL, /* GetInHardware - default false */
    NULL, /* EncryptInit - default errs */
    pem_mdMechanismRSA_DecryptInit,
    NULL, /* DigestInit - default errs */
    pem_mdMechanismRSA_SignInit,
    NULL, /* VerifyInit - default errs */
    pem_mdMechanismRSA_SignInit,        /* SignRecoverInit */
    NULL, /* VerifyRecoverInit - default errs */
    NULL, /* GenerateKey - default errs */
    NULL, /* GenerateKeyPair - default errs */
    NULL, /* GetWrapKeyLength - default errs */
    NULL, /* WrapKey - default errs */
    NULL, /* UnwrapKey - default errs */
    NULL, /* DeriveKey - default errs */
    (void *) NULL /* null terminator */
};
