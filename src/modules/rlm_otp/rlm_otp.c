/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License, version 2 if the
 *   License as published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
 
/**
 * $Id$
 * @file rlm_otp.c
 * @brief One time password implementation.
 *
 * @copyright 2013 Network RADIUS SARL
 * @copyright 2000,2001,2002,2013  The FreeRADIUS server project
 * @copyright 2005-2007 TRI-D Systems, Inc.
 * @copyright 2001,2002  Google, Inc.
 */
#include <freeradius-devel/ident.h>
RCSID("$Id$")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/modules.h>

#include "extern.h"
#include "otp.h"

/* Global data */
static uint8_t hmac_key[16];	//!< to protect State attribute.
static int ninstance = 0;	//!< Number of instances, for global init.

/* A mapping of configuration file names to internal variables. */
static const CONF_PARSER module_config[] = {
	{ "otpd_rp", PW_TYPE_STRING_PTR, offsetof(otp_option_t, otpd_rp),
	  NULL, OTP_OTPD_RP },
	{ "challenge_prompt", PW_TYPE_STRING_PTR,offsetof(otp_option_t, chal_prompt),
	  NULL, OTP_CHALLENGE_PROMPT },
	{ "challenge_length", PW_TYPE_INTEGER, offsetof(otp_option_t, challenge_len),
	  NULL, "6" },
	{ "challenge_delay", PW_TYPE_INTEGER, offsetof(otp_option_t, challenge_delay),
	  NULL, "30" },
	{ "allow_sync", PW_TYPE_BOOLEAN, offsetof(otp_option_t, allow_sync),
	  NULL, "yes" },
	{ "allow_async", PW_TYPE_BOOLEAN, offsetof(otp_option_t, allow_async),
	  NULL, "no" },

	{ "mschapv2_mppe", PW_TYPE_INTEGER,
	  offsetof(otp_option_t, mschapv2_mppe_policy), NULL, "2" },
	{ "mschapv2_mppe_bits", PW_TYPE_INTEGER,
	  offsetof(otp_option_t, mschapv2_mppe_types), NULL, "2" },
	{ "mschap_mppe", PW_TYPE_INTEGER,
	  offsetof(otp_option_t, mschap_mppe_policy), NULL, "2" },
	{ "mschap_mppe_bits", PW_TYPE_INTEGER,
	  offsetof(otp_option_t, mschap_mppe_types), NULL, "2" },

	{ NULL, -1, 0, NULL, NULL }		/* end the list */
};


/*
 *	Per-instance initialization
 */
static int otp_instantiate(CONF_SECTION *conf, void **instance)
{
	otp_option_t *opt;

	/* Set up a storage area for instance data. */
	opt = rad_calloc(sizeof(*opt));

	/* If the configuration parameters can't be parsed, then fail. */
	if (cf_section_parse(conf, opt, module_config) < 0) {
		free(opt);
		return -1;
	}

	/* Onetime initialization. */
	if (!ninstance) {
		/* Generate a random key, used to protect the State attribute. */
		otp_get_random(hmac_key, sizeof(hmac_key));

		/* Initialize the passcode encoding/checking functions. */
		otp_pwe_init();

		/*
		 * Don't do this again.
		 * Only the main thread instantiates and detaches instances,
		 * so this does not need mutex protection.
		 */
		ninstance++;
	}

	/* Verify ranges for those vars that are limited. */
	if ((opt->challenge_len < 5) ||
	    (opt->challenge_len > OTP_MAX_CHALLENGE_LEN)) {
		opt->challenge_len = 6;
		
		radlog(L_ERR, "rlm_otp: %s: invalid challenge_length, "
		       "range 5-%d, using default of 6", __func__,
		       OTP_MAX_CHALLENGE_LEN);
	}

	if (!opt->allow_sync && !opt->allow_async) {
		radlog(L_ERR, "rlm_otp: %s: at least one of {allow_async, "
		       "allow_sync} must be set", __func__);
		free(opt);
		
		return -1;
	}

	if ((opt->mschapv2_mppe_policy > 2) ||
	    (opt->mschapv2_mppe_policy < 0)) {
		opt->mschapv2_mppe_policy = 2;
		
		radlog(L_ERR, "rlm_otp: %s: invalid value for mschapv2_mppe, "
		       "using default of 2", __func__);
	}

	if ((opt->mschapv2_mppe_types > 2) || (opt->mschapv2_mppe_types < 0)) {
		opt->mschapv2_mppe_types = 2;
		radlog(L_ERR, "rlm_otp: %s: invalid value for "
		       "mschapv2_mppe_bits, using default of 2", __func__);
	}

	if ((opt->mschap_mppe_policy > 2) || (opt->mschap_mppe_policy < 0)) {
		opt->mschap_mppe_policy = 2;
		radlog(L_ERR, "rlm_otp: %s: invalid value for mschap_mppe, "
		       "using default of 2", __func__);
  	}

  	if (opt->mschap_mppe_types != 2) {
		opt->mschap_mppe_types = 2;
		radlog(L_ERR, "rlm_otp: %s: invalid value for "
		       "mschap_mppe_bits, using default of 2", __func__);
	}

	/* set the instance name (for use with authorize()) */
	opt->name = cf_section_name2(conf);
	if (!opt->name) {
    		opt->name = cf_section_name1(conf);
    	}
    	
  	if (!opt->name) {
		radlog(L_ERR|L_CONS, "rlm_otp: %s: no instance name "
		       "(this can't happen)", __func__);
    		free(opt);
    		return -1;
	}

	*instance = opt;
	
	return 0;
}

/* 
 *	Generate a challenge to be presented to the user.
 */
static rlm_rcode_t otp_authorize(void *instance, REQUEST *request)
{
	otp_option_t *inst = (otp_option_t *) instance;

	char challenge[OTP_MAX_CHALLENGE_LEN + 1];	/* +1 for '\0' terminator */
	int auth_type_found;

	/* Early exit if Auth-Type != inst->name */
	{
		VALUE_PAIR *vp;

		auth_type_found = 0;
		vp = pairfind(request->config_items, PW_AUTHTYPE, 0, TAG_ANY);
		if (vp) {
      			auth_type_found = 1;
      			if (strcmp(vp->vp_strvalue, inst->name)) {
        			return RLM_MODULE_NOOP;
    			}	
  		}
  	}

	/* The State attribute will be present if this is a response. */
	if (pairfind(request->packet->vps, PW_STATE, 0, TAG_ANY) != NULL) {
		DEBUG("rlm_otp: autz: Found response to Access-Challenge");
		
		return RLM_MODULE_OK;
	}

	/* User-Name attribute required. */
	if (!request->username) {
		radlog(L_AUTH, "rlm_otp: %s: Attribute \"User-Name\" "
		       "required for authentication.", __func__);
		       
		return RLM_MODULE_INVALID;
	}

	if (otp_pwe_present(request) == 0) {
		radlog(L_AUTH, "rlm_otp: %s: Attribute "
		       "\"User-Password\" or equivalent required "
		       "for authentication.", __func__);
		       
		return RLM_MODULE_INVALID;
	}

	/*
	 * 	We used to check for special "challenge" and "resync" passcodes
	 * 	here, but these are complicated to explain and application is
	 * 	limited.  More importantly, since we've removed all actual OTP
	 * 	code (now we ask otpd), it's awkward for us to support them.
	 * 	Should the need arise to reinstate these options, the most 
	 *	likely choice is to duplicate some otpd code here.
	 */
	if (inst->allow_sync && !inst->allow_async) {
		/* This is the token sync response. */
		if (!auth_type_found) {
			pairadd(&request->config_items,
				pairmake("Auth-Type", inst->name, T_OP_EQ));
		}

		return RLM_MODULE_OK;
	}

	/*
	 *	Generate a random challenge.
	 */
	otp_async_challenge(challenge, inst->challenge_len);

	/*
	 *	Create the State attribute, which will be returned to
	 *	us along with the response.  
	 *
	 *	We will need this to verify the response. 
	 *
	 *	It must be hmac protected to prevent insertion of arbitrary
	 *	State by an inside attacker.
	 *
	 *	If we won't actually use the State (server config doesn't 
	 *	allow async), we just use a trivial State.
	 *
	 *	We always create at least a trivial State, so otp_authorize()
	 *	can quickly pass on to otp_authenticate().
	 */
	{
		int32_t now = htonl(time(NULL)); //!< Low-order 32 bits on LP64.

		char gen_state[OTP_MAX_RADSTATE_LEN];
		size_t len;
		VALUE_PAIR *vp;

		len = otp_gen_state(gen_state, challenge, inst->challenge_len,
				    0, now, hmac_key);
		
		vp = paircreate(PW_STATE, 0);	
		if (!vp) {
			return RLM_MODULE_FAIL;
		}

		memcpy(vp->vp_octets, gen_state, len);	
		vp->length = len;
		
		pairadd(&request->reply->vps, vp);
	}

	/*
	 *	Add the challenge to the reply.
	 */
	{
		VALUE_PAIR *vp;
		char buff[1024];
		size_t len;

		/*
		 *	First add the internal OTP challenge attribute to
		 *	the reply list.
		 */
		vp = paircreate(PW_OTP_CHALLENGE, 0);
		if (!vp) {
			return RLM_MODULE_FAIL;
		}
		
		memcpy(vp->vp_strvalue, challenge, inst->challenge_len);	
		vp->length = inst->challenge_len;
		vp->op = T_OP_SET;
	
		pairadd(&request->reply->vps, vp);
		
		/*
		 *	Then add the message to the user to they known
		 *	what the challenge value is.
		 */
		
		len = radius_xlat(buff, sizeof(buff), inst->chal_prompt,
				  request, NULL, NULL);
		if (len == 0) {
			return RLM_MODULE_FAIL;
		}
		
		vp = paircreate(PW_REPLY_MESSAGE, 0);
		if (!vp) {
			return RLM_MODULE_FAIL;
		}
		
		memcpy(vp->vp_strvalue, challenge, len);	
		vp->length = inst->challenge_len;
		vp->op = T_OP_SET;
		
		pairadd(&request->reply->vps, vp);
	}

	/*
	 *	Mark the packet as an Access-Challenge packet.
	 * 	The server will take care of sending it to the user.
	 */
	request->reply->code = PW_ACCESS_CHALLENGE;
	
	DEBUG("rlm_otp: Sending Access-Challenge.");

	if (!auth_type_found) {
		pairadd(&request->config_items,
			pairmake("Auth-Type", inst->name, T_OP_EQ));
	}
	
	return RLM_MODULE_HANDLED;
}


/*
 *	Verify the response entered by the user.
 */
static rlm_rcode_t otp_authenticate(void *instance, REQUEST *request)
{
	otp_option_t *inst = (otp_option_t *) instance;

	const char *username;
	int rc;
	otp_pwe_t pwe;
	VALUE_PAIR *vp;
	
	char challenge[OTP_MAX_CHALLENGE_LEN];	/* cf. authorize() */
	char passcode[OTP_MAX_PASSCODE_LEN + 1];

	challenge[0] = '\0';	/* initialize for otp_pw_valid() */

	/* User-Name attribute required. */
	if (!request->username) {
		radlog(L_AUTH, "rlm_otp: %s: Attribute \"User-Name\" required "
	  	       "for authentication.", __func__);
	  	       
		return RLM_MODULE_INVALID;
	}
	
	username = request->username->vp_strvalue;
	
	pwe = otp_pwe_present(request);
	if (pwe == 0) {
		radlog(L_AUTH, "rlm_otp: %s: Attribute \"User-Password\" "
		       "or equivalent required for authentication.", __func__);
		       
		return RLM_MODULE_INVALID;
	}

	/* Add a message to the auth log. */
	pairadd(&request->packet->vps, pairmake("Module-Failure-Message",
		"rlm_otp", T_OP_EQ));
	pairadd(&request->packet->vps, pairmake("Module-Success-Message",
		"rlm_otp", T_OP_EQ));

	/*
	 *	Retrieve the challenge (from State attribute).
	 */
	vp = pairfind(request->packet->vps, PW_STATE, 0, TAG_ANY);
	if (vp) {
		char	gen_state[OTP_MAX_RADSTATE_LEN]; //!< State as hexits
		uint8_t	bin_state[OTP_MAX_RADSTATE_LEN];
	
		int32_t	then;		//!< State timestamp.
		size_t	elen;		//!< Expected State length.
		size_t	len;

		/*
		 *	Set expected State length (see otp_gen_state()) 
		 */
		elen = (inst->challenge_len * 2) + 8 + 8 + 32;

		if (vp->length != elen) {
			radlog(L_AUTH, "rlm_otp: %s: bad radstate for [%s]: "
			       "length", __func__, username);

			return RLM_MODULE_INVALID;
		}

		/*
		 *	Verify the state.
		 */

		/*
		 *	Convert vp state (ASCII encoded hexits in opaque bin
		 *	string) back to binary.
		 *
		 *	There are notes in otp_radstate as to why the state
		 *	value is encoded as hexits.
		 */
		len = fr_hex2bin(vp->vp_strvalue, bin_state, vp->length);
		if (len != (vp->length / 2)) {
			radlog(L_AUTH, "rlm_otp: %s: bad radstate for [%s]: "
			       "not hex", __func__, username);
		       
			return RLM_MODULE_INVALID;
		}

		/*
		 *	Extract data from State
		 */
		memcpy(challenge, bin_state, inst->challenge_len);

		/*
		 *	Skip flag data
		 */
		memcpy(&then, bin_state + inst->challenge_len + 4, 4);

		/*
		 *	Generate new state from returned input data
		 */
		otp_gen_state(gen_state, challenge, inst->challenge_len, 0,
			      then, hmac_key);

		/*
		 *	Compare generated state (in hex form) 
		 *	against generated state (in hex form) 
		 *	to verify hmac.
		 */
		if (memcmp(gen_state, vp->vp_octets, vp->length)) {
			radlog(L_AUTH, "rlm_otp: %s: bad radstate for [%s]: "
			       "hmac", __func__, username);
	       
			return RLM_MODULE_REJECT;
		}

		/*
		 *	State is valid, but check expiry.
		 */
		then = ntohl(then);
		if (time(NULL) - then > inst->challenge_delay) {
			radlog(L_AUTH, "rlm_otp: %s: bad radstate for [%s]: "
			       "expired", __func__, username);

			return RLM_MODULE_REJECT;
		}
	}
	
	/* do it */
	rc = otp_pw_valid(request, pwe, challenge, inst, passcode);

	/* Add MPPE data as needed. */
	if (rc == RLM_MODULE_OK) {
		otp_mppe(request, pwe, inst, passcode);
	}

	return rc;
}


/*
 *	Per-instance destruction
 */
static int otp_detach(void *instance)
{
	free(instance);
	/*
	* Only the main thread instantiates and detaches instances,
	* so this does not need mutex protection.
	*/
	if (--ninstance == 0) {
		(void) memset(hmac_key, 0, sizeof(hmac_key));
	}
	
	return 0;
}

/*
 *	If the module needs to temporarily modify it's instantiation
 *	data, the type should be changed to RLM_TYPE_THREAD_UNSAFE.
 *	The server will then take care of ensuring that the module
 *	is single-threaded.
 */
module_t rlm_otp = {
	RLM_MODULE_INIT,
	"otp",
	RLM_TYPE_THREAD_SAFE,		/* type */
	otp_instantiate,		/* instantiation */
	otp_detach,			/* detach */
	{
		otp_authenticate,	/* authentication */
		otp_authorize,		/* authorization */
		NULL,			/* preaccounting */
		NULL,			/* accounting */
		NULL,			/* checksimul */
		NULL,			/* pre-proxy */
		NULL,			/* post-proxy */
		NULL			/* post-auth */
	},
};
