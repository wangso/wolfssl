/* dh.c
 *
 * Copyright (C) 2006-2016 wolfSSL Inc.
 *
 * This file is part of wolfSSL.
 *
 * wolfSSL is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * wolfSSL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA
 */


#ifdef HAVE_CONFIG_H
    #include <config.h>
#endif

#include <wolfssl/wolfcrypt/settings.h>

#ifndef NO_DH

#include <wolfssl/wolfcrypt/dh.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/logging.h>

#ifdef NO_INLINE
    #include <wolfssl/wolfcrypt/misc.h>
#else
    #define WOLFSSL_MISC_INCLUDED
    #include <wolfcrypt/src/misc.c>
#endif


#if !defined(USER_MATH_LIB) && !defined(WOLFSSL_DH_CONST)
    #include <math.h>
    #define XPOW(x,y) pow((x),(y))
    #define XLOG(x)   log((x))
#else
    /* user's own math lib */
#endif


int wc_InitDhKey(DhKey* key)
{
    int ret = 0;

    if (key == NULL)
        return BAD_FUNC_ARG;

    if (mp_init_multi(&key->p, &key->g, NULL, NULL, NULL, NULL) != MP_OKAY)
        ret = MEMORY_E;

    return ret;
}


void wc_FreeDhKey(DhKey* key)
{
    if (key) {
    #ifndef USE_FAST_MATH
        mp_clear(&key->p);
        mp_clear(&key->g);
    #endif
    }
}


/* if defined to not use floating point values do not compile in */
#ifndef WOLFSSL_DH_CONST
static word32 DiscreteLogWorkFactor(word32 n)
{
    /* assuming discrete log takes about the same time as factoring */
    if (n<5)
        return 0;
    else
        return (word32)(2.4 * XPOW((double)n, 1.0/3.0) *
                XPOW(XLOG((double)n), 2.0/3.0) - 5);
}
#endif /* WOLFSSL_DH_CONST*/


/* if not using fixed points use DiscreteLogWorkFactor function for unsual size
   otherwise round up on size needed */
#ifndef WOLFSSL_DH_CONST
    #define WOLFSSL_DH_ROUND(x)
#else
    #define WOLFSSL_DH_ROUND(x) \
        do {                    \
            if (x % 128) {      \
                x &= 0xffffff80;\
                x += 128;       \
            }                   \
        }                       \
        while (0)
#endif


static int GeneratePrivate(DhKey* key, WC_RNG* rng, byte* priv, word32* privSz)
{
    int ret;
    word32 sz = mp_unsigned_bin_size(&key->p);

    /* Table of predetermined values from the operation
       2 * DiscreteLogWorkFactor(sz * WOLFSSL_BIT_SIZE) / WOLFSSL_BIT_SIZE + 1
       Sizes in table checked against RFC 3526
     */
    WOLFSSL_DH_ROUND(sz); /* if using fixed points only, then round up */
    switch (sz) {
        case 128:  sz = 21; break;
        case 256:  sz = 29; break;
        case 384:  sz = 34; break;
        case 512:  sz = 39; break;
        case 640:  sz = 42; break;
        case 768:  sz = 46; break;
        case 896:  sz = 49; break;
        case 1024: sz = 52; break;
        default:
            #ifndef WOLFSSL_DH_CONST
                /* if using floating points and size of p is not in table */
                sz = min(sz, 2 * DiscreteLogWorkFactor(sz * WOLFSSL_BIT_SIZE) /
                                           WOLFSSL_BIT_SIZE + 1);
                break;
            #else
                return BAD_FUNC_ARG;
            #endif
    }

    ret = wc_RNG_GenerateBlock(rng, priv, sz);
    if (ret != 0)
        return ret;

    priv[0] |= 0x0C;

    *privSz = sz;

    return 0;
}


static int GeneratePublic(DhKey* key, const byte* priv, word32 privSz,
                          byte* pub, word32* pubSz)
{
    int ret = 0;

    mp_int x;
    mp_int y;

    if (mp_init_multi(&x, &y, 0, 0, 0, 0) != MP_OKAY)
        return MP_INIT_E;

    if (mp_read_unsigned_bin(&x, priv, privSz) != MP_OKAY)
        ret = MP_READ_E;

    if (ret == 0 && mp_exptmod(&key->g, &x, &key->p, &y) != MP_OKAY)
        ret = MP_EXPTMOD_E;

    if (ret == 0 && mp_to_unsigned_bin(&y, pub) != MP_OKAY)
        ret = MP_TO_E;

    if (ret == 0)
        *pubSz = mp_unsigned_bin_size(&y);

    mp_clear(&y);
    mp_clear(&x);

    return ret;
}


int wc_DhGenerateKeyPair(DhKey* key, WC_RNG* rng, byte* priv, word32* privSz,
                      byte* pub, word32* pubSz)
{
    int ret = GeneratePrivate(key, rng, priv, privSz);

    return (ret != 0) ? ret : GeneratePublic(key, priv, *privSz, pub, pubSz);
}


/* Check DH Public Key for invalid numbers
 *
 * key   DH key group parameters.
 * pub   Public Key.
 * pubSz Public Key size.
 *
 *  returns 0 on success or error code
 */
int wc_DhCheckPubKey(DhKey* key, const byte* pub, word32 pubSz)
{
    int ret = 0;

    mp_int x;
    mp_int y;

    if (key == NULL || pub == NULL) {
        return BAD_FUNC_ARG;
    }

    if (mp_init_multi(&x, &y, NULL, NULL, NULL, NULL) != MP_OKAY) {
        return MP_INIT_E;
    }

    if (mp_read_unsigned_bin(&x, pub, pubSz) != MP_OKAY) {
        ret = MP_READ_E;
    }

    /* pub should not be 0 or 1 */
    if (ret == 0 && mp_cmp_d(&x, 2) == MP_LT) {
        ret = MP_CMP_E;
    }

    /* pub shouldn't be greater than or equal to p - 1 */
    if (ret == 0 && mp_copy(&key->p, &y) != MP_OKAY) {
        ret = MP_INIT_E;
    }
    if (ret == 0 && mp_sub_d(&y, 2, &y) != MP_OKAY) {
        ret = MP_SUB_E;
    }
    if (ret == 0 && mp_cmp(&x, &y) == MP_GT) {
        ret = MP_CMP_E;
    }

    mp_clear(&y);
    mp_clear(&x);

    return ret;
}


int wc_DhAgree(DhKey* key, byte* agree, word32* agreeSz, const byte* priv,
            word32 privSz, const byte* otherPub, word32 pubSz)
{
    int ret = 0;

    mp_int x;
    mp_int y;
    mp_int z;

    if (wc_DhCheckPubKey(key, otherPub, pubSz) != 0) {
        WOLFSSL_MSG("wc_DhAgree wc_DhCheckPubKey failed");
        return DH_CHECK_PUB_E;
    }

    if (mp_init_multi(&x, &y, &z, 0, 0, 0) != MP_OKAY)
        return MP_INIT_E;

    if (mp_read_unsigned_bin(&x, priv, privSz) != MP_OKAY)
        ret = MP_READ_E;

    if (ret == 0 && mp_read_unsigned_bin(&y, otherPub, pubSz) != MP_OKAY)
        ret = MP_READ_E;

    if (ret == 0 && mp_exptmod(&y, &x, &key->p, &z) != MP_OKAY)
        ret = MP_EXPTMOD_E;

    if (ret == 0 && mp_to_unsigned_bin(&z, agree) != MP_OKAY)
        ret = MP_TO_E;

    if (ret == 0)
        *agreeSz = mp_unsigned_bin_size(&z);

    mp_clear(&z);
    mp_clear(&y);
    mp_clear(&x);

    return ret;
}


/* not in asn anymore since no actual asn types used */
int wc_DhSetKey(DhKey* key, const byte* p, word32 pSz, const byte* g,
                word32 gSz)
{
    if (key == NULL || p == NULL || g == NULL || pSz == 0 || gSz == 0)
        return BAD_FUNC_ARG;

    /* may have leading 0 */
    if (p[0] == 0) {
        pSz--; p++;
    }

    if (g[0] == 0) {
        gSz--; g++;
    }

    if (mp_init(&key->p) != MP_OKAY)
        return MP_INIT_E;
    if (mp_read_unsigned_bin(&key->p, p, pSz) != 0) {
        mp_clear(&key->p);
        return ASN_DH_KEY_E;
    }

    if (mp_init(&key->g) != MP_OKAY) {
        mp_clear(&key->p);
        return MP_INIT_E;
    }
    if (mp_read_unsigned_bin(&key->g, g, gSz) != 0) {
        mp_clear(&key->g);
        mp_clear(&key->p);
        return ASN_DH_KEY_E;
    }

    return 0;
}


#endif /* NO_DH */

