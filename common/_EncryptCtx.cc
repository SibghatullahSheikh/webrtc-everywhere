/* Copyright(C) 2014 Sarandogou <https://github.com/sarandogou/webrtc-everywhere> */
#include "_EncryptCtx.h"
#include "_Utils.h"
#include "_Debug.h"

#if 1
#include "third_party/nss/nss/lib/nss.h"
#include "third_party/nss/nss/lib/pk11wrap/pk11pub.h"
#include "prerror.h"
#else
typedef int PRIntn;
typedef int PRInt32;
typedef unsigned long CK_ULONG;
typedef PRInt32 PRErrorCode;
typedef PRIntn PRBool;
typedef CK_ULONG CK_MECHANISM_TYPE;
#define SECSuccess 0
#define siBuffer 0
#define PK11_OriginUnwrap 4
#define CKM_DES_CBC_PAD 0x00000125
#define CKA_ENCRYPT 0x00000104
#define CKA_DECRYPT 0x00000105

struct SECItemStr {
	enum SECItemType type;
	unsigned char *data;
	unsigned int len;
};

extern "C" {
	extern enum _SECStatus NSS_NoDB_Init(const char *configdir);
	extern void PK11_FreeSymKey(struct PK11SymKeyStr *key);
	extern struct PK11SlotInfoStr *PK11_GetBestSlot(CK_MECHANISM_TYPE type, void *wincx);
	void PK11_DestroyContext(struct PK11ContextStr *context, PRBool freeit);;
	extern PRErrorCode PR_GetError(void);
}

#endif

static bool g_Initialized = false;
static CK_MECHANISM_TYPE  g_cipherMech = CKM_DES_CBC_PAD;

// Code based on https://developer.mozilla.org/en-US/docs/Mozilla/Projects/NSS/nss_sample_code/NSS_Sample_Code_sample2

_EncryptCtx::_EncryptCtx()
	: m_SymKey(NULL)
	, m_SecParam(NULL)
    , m_Slot(NULL)
{
	if (!g_Initialized) {
		enum _SECStatus rv = NSS_NoDB_Init(".");
		if (rv != SECSuccess) {
			WE_DEBUG_ERROR("NSS initialization failed (err %s)", PR_ErrorToName(PR_GetError()));
			return;
		}
		/* choose mechanism: CKM_DES_CBC_PAD, CKM_DES3_ECB, CKM_DES3_CBC.....
		* Note that some mechanisms (*_PAD) imply the padding is handled for you
		* by NSS. If you choose something else, then data padding is the
		* application's responsibility
		*/
		g_cipherMech = CKM_DES_CBC_PAD;
		g_Initialized = true;
	}

	// Get Key and IV
	const unsigned char* key_ptr = NULL;
	size_t key_size = 0;
	const unsigned char* iv_ptr = NULL;
	size_t iv_size = 0;
	if (_Utils::FileConfigGetKeyAndIV(key_ptr, key_size, iv_ptr, iv_size) != WeError_Success) {
		return;
	}
    
	SECItem keyItem, ivItem;

	m_Slot = PK11_GetBestSlot(g_cipherMech, NULL);
	/* slot = PK11_GetInternalKeySlot(); is a simpler alternative but in
	* theory, it *may not* return the optimal slot for the operation. For
	* DES ops, Internal slot is typically the best slot
	*/
	if (m_Slot == NULL) {
		WE_DEBUG_ERROR("Unable to find security device (err %s)", PR_ErrorToName(PR_GetError()));
		return;
	}

	/* NSS passes blobs around as SECItems. These contain a pointer to
	* data and a length. Turn the raw key into a SECItem. */
	keyItem.type = siBuffer;
	keyItem.data = (unsigned char*)key_ptr;
	keyItem.len = key_size;

	/* Turn the raw key into a key object. We use PK11_OriginUnwrap
	* to indicate the key was unwrapped - which is what should be done
	* normally anyway - using raw keys isn't a good idea */
	m_SymKey = PK11_ImportSymKey(m_Slot, g_cipherMech, PK11_OriginUnwrap, CKA_ENCRYPT, &keyItem, NULL);
	if (m_SymKey == NULL) {
		WE_DEBUG_ERROR("Failure to import key into NSS (err %s)", PR_ErrorToName(PR_GetError()));
		return;
	}

	/* set up the PKCS11 encryption paramters.
	* when not using CBC mode, ivItem.data and ivItem.len can be 0, or you
	* can simply pass NULL for the iv parameter in PK11_ParamFromIV func
	*/
	ivItem.type = siBuffer;
	ivItem.data = (unsigned char*)iv_ptr;
	ivItem.len = iv_size;
	m_SecParam = PK11_ParamFromIV(g_cipherMech, &ivItem);
	if (m_SecParam == NULL) {
		WE_DEBUG_ERROR("Failure to set up PKCS11 param (err %s)", PR_ErrorToName(PR_GetError()));
	}
}

_EncryptCtx::~_EncryptCtx()
{
	m_SecParam = NULL;
	if (m_SymKey) {
		PK11_FreeSymKey(m_SymKey);
		m_SymKey = NULL;
	}
    if (m_Slot) {
        PK11_FreeSlot(m_Slot);
    }
}

WeError _EncryptCtx::Op(const cpp11::shared_ptr<_Buffer> &in, cpp11::shared_ptr<_Buffer> &out, bool encrypt)
{
#if 0
	out = in;
	return WeError_Success;
#else
	WeError err = WeError_System;
	int out_maxlen, result_len;
	unsigned char* out_buff = NULL;
	PK11Context* EncContext = NULL;
	int out_len, out2_len;
	SECStatus rv;

	if (!m_SymKey || !m_SecParam) {
		WE_DEBUG_ERROR("Not initialized");
		err = WeError_NotInitialized;
		goto bail;
	}
	if (!in || !in->getPtr() || !in->getSize()) {
		WE_DEBUG_ERROR("Invalid parameter");
		err = WeError_InvalidArgument;
		goto bail;
	}

	EncContext = PK11_CreateContextBySymKey(g_cipherMech, encrypt ? CKA_ENCRYPT : CKA_DECRYPT, m_SymKey, m_SecParam);
	if (!EncContext) {
		WE_DEBUG_ERROR("PK11_CreateContextBySymKey(encrypt=%s) failed", encrypt ? "true" : "false");
		err = WeError_System;
		goto bail;
	}

	out_maxlen = in->getSize() + (encrypt ? (8 + PK11_GetBlockSize(g_cipherMech, m_SecParam)) : 0);
	out_buff = (unsigned char*)malloc(out_maxlen);
	out_len = 0, out2_len = 0;
	if (!out_buff) {
		WE_DEBUG_ERROR("Failed to allocate buffer with size = %u", out_maxlen);
		err = WeError_OutOfMemory;
		goto bail;
	}
	rv = PK11_CipherOp(EncContext, out_buff, &out_len, out_maxlen, (const unsigned char*)in->getPtr(), in->getSize());
	if (rv != SECSuccess) {
		WE_DEBUG_ERROR("PK11_CipherOp failed: %s", PR_ErrorToName(PR_GetError()));
		err = WeError_System;
		goto bail;
	}
	rv = PK11_DigestFinal(EncContext, out_buff + out_len, (unsigned int*)&out2_len, out_maxlen - out_len);
	if (rv != SECSuccess) {
		WE_DEBUG_ERROR("PK11_DigestFinal failed: %s", PR_ErrorToName(PR_GetError()));
		err = WeError_System;
		goto bail;
	}
    result_len = out_len + out2_len;
	out = cpp11::shared_ptr<_Buffer>(new _Buffer(out_buff, result_len));
	if (out && out->getPtr() && out->getSize() == result_len) {
		err = WeError_Success;
	}

bail:
	if (out_buff) {
		free(out_buff);
	}
	if (EncContext) {
		PK11_DestroyContext(EncContext, PR_TRUE);
	}

	return err;
#endif
}

WeError _EncryptCtx::Encrypt(const cpp11::shared_ptr<_Buffer> &in, cpp11::shared_ptr<_Buffer> &out)
{
	return this->Op(in, out, true);
}

WeError _EncryptCtx::Decrypt(const cpp11::shared_ptr<_Buffer> &in, cpp11::shared_ptr<_Buffer> &out)
{
	return this->Op(in, out, false);
}