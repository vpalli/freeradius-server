/*
 * valuepair.c	Functions to handle VALUE_PAIRs
 *
 * Version:	$Id$
 *
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2000,2006  The FreeRADIUS server project
 */

#include <freeradius-devel/ident.h>
RCSID("$Id$")

#include	<freeradius-devel/libradius.h>

#include	<ctype.h>

#ifdef HAVE_MALLOC_H
#  include	<malloc.h>
#endif

#ifdef HAVE_PCREPOSIX_H
#define WITH_REGEX
#  include	<pcreposix.h>
#else
#ifdef HAVE_REGEX_H
#define WITH_REGEX
#  include	<regex.h>
#endif
#endif

static const char *months[] = {
        "jan", "feb", "mar", "apr", "may", "jun",
        "jul", "aug", "sep", "oct", "nov", "dec" };

/** Dynamically allocate a new attribute
 *
 * Allocates a new attribute and a new dictionary attr if no DA is provided.
 *
 * @param[in] da Specifies the dictionary attribute to build the VP from.
 * @return a new value pair or NULL if an error occurred.
 */
VALUE_PAIR *pairalloc(const DICT_ATTR *da)
{
	VALUE_PAIR *vp;

	/*
	 *	Caller must specify a da else we don't know what the attribute
	 *	type is.
	 */
	if (!da) return NULL;

	vp = malloc(sizeof(*vp));
	if (!vp) {
		fr_strerror_printf("Out of memory");
		return NULL;
	}
	memset(vp, 0, sizeof(*vp));
	
	vp->da = da;
	vp->op = T_OP_EQ;

	return vp;
}

/** Create a new valuepair and dictionary entry
 *
 * For use where there is no dictionary entry for the valuepair being
 * created.
 *
 * @param[in] attr number.
 * @param[in] vendor number.
 * @return the new valuepair or NULL on error.
 */
VALUE_PAIR *paircreate_unknown(unsigned int attr, unsigned int vendor)
{
	const DICT_ATTR *da;
	
	da = dict_attrunknown(attr, vendor);
	if (!da) {
		return NULL;	
	}
	
	return pairalloc(da);
}

/** Create a new valuepair
 * 
 * For use when the attribute/vendor are known to exist. Will lookup attr
 * and vendor in dictionary and return NULL if they're not found.
 * 
 * @param[in] attr number.
 * @param[in] vendor number.
 * @return the new valuepair or NULL on error.
 */
VALUE_PAIR *paircreate(unsigned int attr, unsigned int vendor)
{
	const DICT_ATTR *da;

	da = dict_attrbyvalue(attr, vendor);
	if (!da) {
		return NULL;
	}

	return pairalloc(da);
}

/** Free memory used by a single valuepair.
 * 
 * @todo TLV: needs to die in fire.
 */
void pairbasicfree(VALUE_PAIR *pair)
{
	if (pair->da->type == PW_TYPE_TLV) {
		free(pair->vp_tlv);
	}
	
	/*
	 *	Only frees the da if it's dynamically allocated.
	 */
	dict_attr_free(&(pair->da));
	
	/* clear the memory here */
	memset(pair, 0, sizeof(*pair));
	free(pair);
}

/** Free memory used by a valuepair list.
 * 
 * @todo TLV: needs to free all dependents of each VP freed.
 */
void pairfree(VALUE_PAIR **pair_ptr)
{
	VALUE_PAIR *next, *pair;

	if (!pair_ptr) return;
	pair = *pair_ptr;

	while (pair != NULL) {
		next = pair->next;
		pairbasicfree(pair);
		pair = next;
	}

	*pair_ptr = NULL;
}

/** Mark malformed or unrecognised attributed as unknown
 *
 * @param vp to change DICT_ATTR of.
 * @return 0 on success (or if already unknown) else -1 on error.
 */
int pair2unknown(VALUE_PAIR *vp)
{
	const DICT_ATTR *da;
	
	if (vp->da->flags.is_unknown) {
		return 0;
	}
	
	da = dict_attrunknown(vp->da->attr, vp->da->vendor);
	if (!da) {
		return -1;
	}
	
	vp->da = da;
	
	return 0;
}

/** Find the pair with the matching attribute
 *
 * @todo should take DAs and do a pointer comparison.
 */
VALUE_PAIR *pairfind(VALUE_PAIR *first, unsigned int attr, unsigned int vendor,
		      int8_t tag)
{
	while (first) {
		if ((first->da->attr == attr) && (first->da->vendor == vendor)
		    && ((tag == TAG_ANY) || (first->da->flags.has_tag &&
		        (first->tag == tag)))) {
			return first;
		}
		first = first->next;
	}

	return NULL;
}


/** Delete matching pairs
 *
 * Delete matching pairs from the attribute list.
 * 
 * @param[in,out] first VP in list.
 * @param[in] attr to match.
 * @param[in] vendor to match.
 * @param[in] tag to match. TAG_ANY matches any tag, TAG_UNUSED matches tagless VPs.
 *
 * @todo should take DAs and do a point comparison.
 */
void pairdelete(VALUE_PAIR **first, unsigned int attr, unsigned int vendor,
		int8_t tag)
{
	VALUE_PAIR *i, *next;
	VALUE_PAIR **last = first;

	for(i = *first; i; i = next) {
		next = i->next;
		if ((i->da->attr == attr) && (i->da->vendor == vendor) &&
		    ((tag == TAG_ANY) ||
		     (i->da->flags.has_tag && (i->tag == tag)))) {
			*last = next;
			pairbasicfree(i);
		} else {
			last = &i->next;
		}
	}
}

/** Add a VP to the end of the list.
 *
 * Locates the end of 'first', and links an additional VP 'add' at the end.
 * 
 * @param[in] first VP in linked list. Will add new VP to the end of this list.
 * @param[in] add VP to add to list.
 * @return a copy of the input VP
 */
void pairadd(VALUE_PAIR **first, VALUE_PAIR *add)
{
	VALUE_PAIR *i;

	if (!add) return;

	if (*first == NULL) {
		*first = add;
		return;
	}
	for(i = *first; i->next; i = i->next)
		;
	i->next = add;
}

/** Replace all matching VPs
 *
 * Walks over 'first', and replaces the first VP that matches 'replace'.
 * 
 * @note Memory used by the VP being replaced will be freed.
 * @note Will not work with unknown attributes.
 * 
 * @param[in,out] first VP in linked list. Will search and replace in this list.
 * @param[in] replace VP to replace.
 * @return a copy of the input vp
 */
void pairreplace(VALUE_PAIR **first, VALUE_PAIR *replace)
{
	VALUE_PAIR *i, *next;
	VALUE_PAIR **prev = first;

	if (*first == NULL) {
		*first = replace;
		return;
	}

	/*
	 *	Not an empty list, so find item if it is there, and
	 *	replace it. Note, we always replace the first one, and
	 *	we ignore any others that might exist.
	 */
	for(i = *first; i; i = next) {
		next = i->next;

		/*
		 *	Found the first attribute, replace it,
		 *	and return.
		 */
		if ((i->da == replace->da) &&
		    (!i->da->flags.has_tag || (i->tag == replace->tag))
		) {
			*prev = replace;

			/*
			 *	Should really assert that replace->next == NULL
			 */
			replace->next = next;
			pairbasicfree(i);
			return;
		}

		/*
		 *	Point to where the attribute should go.
		 */
		prev = &i->next;
	}

	/*
	 *	If we got here, we didn't find anything to replace, so
	 *	stopped at the last item, which we just append to.
	 */
	*prev = replace;
}


/** Copy a single valuepair
 *
 * Allocate a new valuepair and copy the da from the old vp.
 *
 * @param[in] vp to copy.
 * @return a copy of the input VP or NULL on error.
 */
VALUE_PAIR *paircopyvp(const VALUE_PAIR *vp)
{
	VALUE_PAIR *n;

	if (!vp) return NULL;
	
	n = malloc(sizeof(*n));
	if (!n) {
		fr_strerror_printf("out of memory");
		return NULL;
	}
	
	memcpy(n, vp, sizeof(*n));
	n->da = dict_attr_copy(vp->da);
	if (!n->da) {
		free(n);
		
		return NULL;
	}
	
	n->next = NULL;

	if ((n->da->type == PW_TYPE_TLV) &&
	    (n->vp_tlv != NULL)) {
		n->vp_tlv = malloc(n->length);
		memcpy(n->vp_tlv, vp->vp_tlv, n->length);
	}

	return n;
}

/** Copy data from one VP to another
 *
 * Allocate a new pair using da, and copy over the value from the specified vp.
 *
 * @todo Should be able to do type conversions.
 * 
 * @param[in] da of new attribute to alloc.
 * @param[in] vp to copy data from.
 * @return the new valuepair.
 */
VALUE_PAIR *paircopyvpdata(const DICT_ATTR *da, const VALUE_PAIR *vp)
{
	VALUE_PAIR *n;

	if (!vp) return NULL;

	if (da->type != vp->da->type) return NULL;
	
	n = pairalloc(da);
	if (!n) {
		return NULL;	
	}
	
	memcpy(&(n->data), &(vp->data), sizeof(n->data));
	
	n->length = vp->length;
	
	if ((n->da->type == PW_TYPE_TLV) &&
	    (n->vp_tlv != NULL)) {
		n->vp_tlv = malloc(n->length);
		memcpy(n->vp_tlv, vp->vp_tlv, n->length);
	}
	
	return n;
}


/** Copy matching pairs
 *
 * Copy pairs of a matching attribute number, vendor number and tag from the
 * the input list to a new list, and returns the head of this list.
 * 
 * @param[in] vp which is head of the input list.
 * @param[in] attr to match, if 0 input list will not be filtered by attr.
 * @param[in] vendor to match.
 * @param[in] tag to match, TAG_ANY matches any tag, TAG_UNUSED matches tagless VPs.
 * @return the head of the new VALUE_PAIR list.
 */
VALUE_PAIR *paircopy2(VALUE_PAIR *vp, unsigned int attr, unsigned int vendor,
		      int8_t tag)
{
	VALUE_PAIR *first, *n, **last;

	first = NULL;
	last = &first;

	while (vp) {
		if ((attr > 0) &&
		    ((vp->da->attr != attr) || (vp->da->vendor != vendor)))
			goto skip;
			
		if ((tag != TAG_ANY) && vp->da->flags.has_tag &&
		    (vp->tag != tag)) {
			goto skip;
		}

		n = paircopyvp(vp);
		if (!n) return first;
		
		*last = n;
		last = &n->next;
		vp = vp->next;
		
		continue;
		
		skip:
		vp = vp->next;
	}
	
	return first;
}


/*
 *	Copy a pairlist.
 */
VALUE_PAIR *paircopy(VALUE_PAIR *vp)
{
	return paircopy2(vp, 0, 0, TAG_ANY);
}

/** Move pairs from source list to destination list respecting operator
 *
 * @note This function does some additional magic that's probably not needed
 *	 in most places. Consider using radius_pairmove in server code.
 *
 * @note pairfree should be called on the head of the source list to free
 *	 unmoved attributes (if they're no longer needed). 
 *
 * @note Does not respect tags when matching.
 * 
 * @param[in,out] to destination list.
 * @param[in,out] from source list.
 *
 * @see radius_pairmove
 */
void pairmove(VALUE_PAIR **to, VALUE_PAIR **from)
{
	VALUE_PAIR **tailto, *i, *j, *next;
	VALUE_PAIR *tailfrom = NULL;
	VALUE_PAIR *found;
	int has_password = 0;

	/*
	 *	First, see if there are any passwords here, and
	 *	point "tailto" to the end of the "to" list.
	 */
	tailto = to;
	for(i = *to; i; i = i->next) {
		if (i->da->attr == PW_USER_PASSWORD ||
		    i->da->attr == PW_CRYPT_PASSWORD)
			has_password = 1;
		tailto = &i->next;
	}

	/*
	 *	Loop over the "from" list.
	 */
	for(i = *from; i; i = next) {
		next = i->next;

		/*
		 *	If there was a password in the "to" list,
		 *	do not move any other password from the
		 *	"from" to the "to" list.
		 */
		if (has_password &&
		    (i->da->attr == PW_USER_PASSWORD ||
		     i->da->attr == PW_CRYPT_PASSWORD)) {
			tailfrom = i;
			continue;
		}

		switch (i->op) {
			/*
			 *	These are COMPARISON attributes
			 *	from a check list, and are not
			 *	supposed to be copied!
			 */
			case T_OP_NE:
			case T_OP_GE:
			case T_OP_GT:
			case T_OP_LE:
			case T_OP_LT:
			case T_OP_CMP_TRUE:
			case T_OP_CMP_FALSE:
			case T_OP_CMP_EQ:
			case T_OP_REG_EQ:
			case T_OP_REG_NE:
				tailfrom = i;
				continue;

			default:
				break;
		}

		/*
		 *	If the attribute is already present in "to",
		 *	do not move it from "from" to "to". We make
		 *	an exception for "Hint" which can appear multiple
		 *	times, and we never move "Fall-Through".
		 */
		if (i->da->attr == PW_FALL_THROUGH ||
		    (i->da->attr != PW_HINT && i->da->attr != PW_FRAMED_ROUTE)) {


			found = pairfind(*to, i->da->attr, i->da->vendor,
					 TAG_ANY);
					 
			switch (i->op) {

			/*
			 *	If matching attributes are found,
			 *	delete them.
			 */
			case T_OP_SUB:		/* -= */
				if (found) {
					if (!i->vp_strvalue[0] ||
					    (strcmp((char *)found->vp_strvalue,
						    (char *)i->vp_strvalue) == 0)){
						pairdelete(to,
							   found->da->attr,
							   found->da->vendor,
							   TAG_ANY);

						/*
						 *	'tailto' may have been
						 *	deleted...
						 */
						tailto = to;
						for(j = *to; j; j = j->next) {
							tailto = &j->next;
						}
					}
				}
				tailfrom = i;
				continue;
				break;

			case T_OP_EQ:		/* = */
				/*
				 *  FIXME: Tunnel attributes with
				 *  different tags are different
				 *  attributes.
				 */
				if (found) {
					tailfrom = i;
					continue; /* with the loop */
				}
				break;

			  /*
			   *  If a similar attribute is found,
			   *  replace it with the new one.  Otherwise,
			   *  add the new one to the list.
			   */
			case T_OP_SET:		/* := */
				if (found) {
					VALUE_PAIR *mynext = found->next;

					/*
					 *	Do NOT call pairdelete()
					 *	here, due to issues with
					 *	re-writing "request->username".
					 *
					 *	Everybody calls pairmove,
					 *	and expects it to work.
					 *	We can't update request->username
					 *	here, so instead we over-write
					 *	the vp that it's pointing to.
					 */
					memcpy(found, i, sizeof(*found));
					found->next = mynext;

					pairdelete(&found->next,
						   found->da->attr,
						   found->da->vendor, TAG_ANY);

					/*
					 *	'tailto' may have been
					 *	deleted...
					 */
					for(j = found; j; j = j->next) {
						tailto = &j->next;
					}
					continue;
				}
				break;

			  /*
			   *  Add the new element to the list, even
			   *  if similar ones already exist.
			   */
			default:
			case T_OP_ADD: /* += */
				break;
			}
		}
		if (tailfrom)
			tailfrom->next = next;
		else
			*from = next;

		/*
		 *	If ALL of the 'to' attributes have been deleted,
		 *	then ensure that the 'tail' is updated to point
		 *	to the head.
		 */
		if (!*to) {
			tailto = to;
		}
		*tailto = i;
		if (i) {
			i->next = NULL;
			tailto = &i->next;
		}
	}
}

/** Move matching pairs
 *
 * Move pairs of a matching attribute number, vendor number and tag from the
 * the input list to the output list.
 *
 * @note pairfree should be called on the head of the old list to free unmoved
 	 attributes (if they're no longer needed). 
 * 
 * @param[in,out] to destination list.
 * @param[in,out] from source list.
 * @param[in] attr to match, if PW_VENDOR_SPECIFIC and vendor 0, only VSAs will
 *	      be copied.
 * @param[in] vendor to match.
 * @param[in] tag to match, TAG_ANY matches any tag, TAG_UNUSED matches tagless VPs.
 */
void pairmove2(VALUE_PAIR **to, VALUE_PAIR **from, unsigned int attr,
	       unsigned int vendor, int8_t tag)
{
	VALUE_PAIR *to_tail, *i, *next;
	VALUE_PAIR *iprev = NULL;

	/*
	 *	Find the last pair in the "to" list and put it in "to_tail".
	 */
	if (*to != NULL) {
		to_tail = *to;
		for(i = *to; i; i = i->next)
			to_tail = i;
	} else
		to_tail = NULL;

	for(i = *from; i; i = next) {
		next = i->next;

		if ((tag != TAG_ANY) && i->da->flags.has_tag &&
		    (i->tag != tag)) {
			continue;
		}
		
		/*
		 *	vendor=0, attr = PW_VENDOR_SPECIFIC means
		 *	"match any vendor attribute".
		 */
		if ((vendor == 0) && (attr == PW_VENDOR_SPECIFIC)) {
			/*
			 *	It's a VSA: move it over.
			 */
			if (i->da->vendor != 0) goto move;

			/*
			 *	It's Vendor-Specific: move it over.
			 */
			if (i->da->attr == attr) goto move;

			/*
			 *	It's not a VSA: ignore it.
			 */
			iprev = i;
			continue;
		}

		/*
		 *	If it isn't an exact match, ignore it.
		 */
		if (!((i->da->vendor == vendor) && (i->da->attr == attr))) {
			iprev = i;
			continue;
		}

	move:
		/*
		 *	Remove the attribute from the "from" list.
		 */
		if (iprev)
			iprev->next = next;
		else
			*from = next;

		/*
		 *	Add the attribute to the "to" list.
		 */
		if (to_tail)
			to_tail->next = i;
		else
			*to = i;
		to_tail = i;
		i->next = NULL;
	}
}


/*
 *	Sort of strtok/strsep function.
 */
static char *mystrtok(char **ptr, const char *sep)
{
	char	*res;

	if (**ptr == 0)
		return NULL;
	while (**ptr && strchr(sep, **ptr))
		(*ptr)++;
	if (**ptr == 0)
		return NULL;
	res = *ptr;
	while (**ptr && strchr(sep, **ptr) == NULL)
		(*ptr)++;
	if (**ptr != 0)
		*(*ptr)++ = 0;
	return res;
}

/*
 *	Turn printable string into time_t
 *	Returns -1 on error, 0 on OK.
 */
static int gettime(const char *valstr, time_t *date)
{
	int		i;
	time_t		t;
	struct tm	*tm, s_tm;
	char		buf[64];
	char		*p;
	char		*f[4];
	char            *tail = '\0';

	/*
	 * Test for unix timestamp date
	 */
	*date = strtoul(valstr, &tail, 10);
	if (*tail == '\0') {
		return 0;
	}

	tm = &s_tm;
	memset(tm, 0, sizeof(*tm));
	tm->tm_isdst = -1;	/* don't know, and don't care about DST */

	strlcpy(buf, valstr, sizeof(buf));

	p = buf;
	f[0] = mystrtok(&p, " \t");
	f[1] = mystrtok(&p, " \t");
	f[2] = mystrtok(&p, " \t");
	f[3] = mystrtok(&p, " \t"); /* may, or may not, be present */
	if (!f[0] || !f[1] || !f[2]) return -1;

	/*
	 *	The time has a colon, where nothing else does.
	 *	So if we find it, bubble it to the back of the list.
	 */
	if (f[3]) {
		for (i = 0; i < 3; i++) {
			if (strchr(f[i], ':')) {
				p = f[3];
				f[3] = f[i];
				f[i] = p;
				break;
			}
		}
	}

	/*
	 *  The month is text, which allows us to find it easily.
	 */
	tm->tm_mon = 12;
	for (i = 0; i < 3; i++) {
		if (isalpha( (int) *f[i])) {
			/*
			 *  Bubble the month to the front of the list
			 */
			p = f[0];
			f[0] = f[i];
			f[i] = p;

			for (i = 0; i < 12; i++) {
				if (strncasecmp(months[i], f[0], 3) == 0) {
					tm->tm_mon = i;
					break;
				}
			}
		}
	}

	/* month not found? */
	if (tm->tm_mon == 12) return -1;

	/*
	 *  The year may be in f[1], or in f[2]
	 */
	tm->tm_year = atoi(f[1]);
	tm->tm_mday = atoi(f[2]);

	if (tm->tm_year >= 1900) {
		tm->tm_year -= 1900;

	} else {
		/*
		 *  We can't use 2-digit years any more, they make it
		 *  impossible to tell what's the day, and what's the year.
		 */
		if (tm->tm_mday < 1900) return -1;

		/*
		 *  Swap the year and the day.
		 */
		i = tm->tm_year;
		tm->tm_year = tm->tm_mday - 1900;
		tm->tm_mday = i;
	}

	/*
	 *  If the day is out of range, die.
	 */
	if ((tm->tm_mday < 1) || (tm->tm_mday > 31)) {
		return -1;
	}

	/*
	 *	There may be %H:%M:%S.  Parse it in a hacky way.
	 */
	if (f[3]) {
		f[0] = f[3];	/* HH */
		f[1] = strchr(f[0], ':'); /* find : separator */
		if (!f[1]) return -1;

		*(f[1]++) = '\0'; /* nuke it, and point to MM:SS */

		f[2] = strchr(f[1], ':'); /* find : separator */
		if (f[2]) {
		  *(f[2]++) = '\0';	/* nuke it, and point to SS */
		  tm->tm_sec = atoi(f[2]);
		}			/* else leave it as zero */

		tm->tm_hour = atoi(f[0]);
		tm->tm_min = atoi(f[1]);
	}

	/*
	 *  Returns -1 on error.
	 */
	t = mktime(tm);
	if (t == (time_t) -1) return -1;

	*date = t;

	return 0;
}

static const char *hextab = "0123456789abcdef";

/*
 *  Parse a string value into a given VALUE_PAIR
 *
 *  FIXME: we probably want to fix this function to accept
 *  octets as values for any type of attribute.  We should then
 *  double-check the parsed value, to be sure it's legal for that
 *  type (length, etc.)
 */
static uint32_t getint(const char *value, char **end)
{
	if ((value[0] == '0') && (value[1] == 'x')) {
		return strtoul(value, end, 16);
	}

	return strtoul(value, end, 10);
}

static int check_for_whitespace(const char *value)
{
	while (*value) {
		if (!isspace((int) *value)) return 0;

		value++;
	}

	return 1;
}

/** Convert a string value into a valuepair
 *
 * Uses the type of valuepair to determine the parsing scheme for the string.
 * Will attempt to convert the string into the data type for a given vp.
 *
 * e.g. '012345' would become integer 12345.
 *
 * @param vp to use as a template.
 * @param value string to parse.
 *
 * @todo change valuepair to da.
 */
VALUE_PAIR *pairparsevalue(VALUE_PAIR *vp, const char *value)
{
	char		*p, *s=0;
	const char	*cp, *cs;
	int		x;
	unsigned long long y;
	size_t		length;
	DICT_VALUE	*dval;

	if (!value) return NULL;

	/*
	 *	Even for integers, dates and ip addresses we
	 *	keep the original string in vp->vp_strvalue.
	 */
	if (vp->da->type != PW_TYPE_TLV) {
		strlcpy(vp->vp_strvalue, value, sizeof(vp->vp_strvalue));
		vp->length = strlen(vp->vp_strvalue);
	}

	switch(vp->da->type) {
	case PW_TYPE_STRING:
		/*
		 *	Do escaping here
		 */
		p = vp->vp_strvalue;
		cp = value;
		length = 0;

		while (*cp && (length < (sizeof(vp->vp_strvalue) - 1))) {
			char c = *cp++;

			if (c == '\\') {
				switch (*cp) {
				case 'r':
					c = '\r';
					cp++;
					break;
				case 'n':
					c = '\n';
					cp++;
					break;
				case 't':
					c = '\t';
					cp++;
					break;
				case '"':
					c = '"';
					cp++;
					break;
				case '\'':
					c = '\'';
					cp++;
					break;
				case '\\':
					c = '\\';
					cp++;
					break;
				case '`':
					c = '`';
					cp++;
					break;
				case '\0':
					c = '\\'; /* no cp++ */
					break;
				default:
					if ((cp[0] >= '0') &&
					    (cp[0] <= '9') &&
					    (cp[1] >= '0') &&
					    (cp[1] <= '9') &&
					    (cp[2] >= '0') &&
					    (cp[2] <= '9') &&
					    (sscanf(cp, "%3o", &x) == 1)) {
						c = x;
						cp += 3;
					} /* else just do '\\' */
				}
			}
			*p++ = c;
			length++;
		}
		vp->vp_strvalue[length] = '\0';
		vp->length = length;
		break;

	case PW_TYPE_IPADDR:
		/*
		 *	It's a comparison, not a real IP.
		 */
		if ((vp->op == T_OP_REG_EQ) ||
		    (vp->op == T_OP_REG_NE)) {
			break;
		}

		/*
		 *	FIXME: complain if hostname
		 *	cannot be resolved, or resolve later!
		 */
		s = NULL;
		p = NULL;
		cs = value;

		{
			fr_ipaddr_t ipaddr;

			if (ip_hton(cs, AF_INET, &ipaddr) < 0) {
				fr_strerror_printf("Failed to find IP address for %s", cs);
				free(s);
				return NULL;
			}

			vp->vp_ipaddr = ipaddr.ipaddr.ip4addr.s_addr;
		}
		free(s);
		vp->length = 4;
		break;

	case PW_TYPE_BYTE:
		vp->length = 1;

		/*
		 *	Note that ALL integers are unsigned!
		 */
		vp->vp_integer = getint(value, &p);
		if (!*p) {
			if (vp->vp_integer > 255) {
				fr_strerror_printf("Byte value \"%s\" is larger than 255", value);
				return NULL;
			}
			break;
		}
		if (check_for_whitespace(p)) break;
		goto check_for_value;

	case PW_TYPE_SHORT:
		/*
		 *	Note that ALL integers are unsigned!
		 */
		vp->vp_integer = getint(value, &p);
		vp->length = 2;
		if (!*p) {
			if (vp->vp_integer > 65535) {
				fr_strerror_printf("Byte value \"%s\" is larger than 65535", value);
				return NULL;
			}
			break;
		}
		if (check_for_whitespace(p)) break;
		goto check_for_value;

	case PW_TYPE_INTEGER:
		/*
		 *	Note that ALL integers are unsigned!
		 */
		vp->vp_integer = getint(value, &p);
		vp->length = 4;
		if (!*p) break;
		if (check_for_whitespace(p)) break;

check_for_value:
	/*
	 *	Look for the named value for the given
	 *	attribute.
	 */
	if ((dval = dict_valbyname(vp->da->attr, vp->da->vendor, value)) == NULL) {
		fr_strerror_printf("Unknown value %s for attribute %s",
			   value, vp->da->name);
		return NULL;
	}
	vp->vp_integer = dval->value;
	break;

	case PW_TYPE_INTEGER64:
		/*
		 *	Note that ALL integers are unsigned!
		 */
		p = vp->vp_strvalue;
		if (sscanf(p, "%llu", &y) != 1) {
			fr_strerror_printf("Invalid value %s for attribute %s",
					   value, vp->da->name);
			return NULL;
		}
		vp->vp_integer64 = y;
		vp->length = 8;
		p += strspn(p, "0123456789");
		if (check_for_whitespace(p)) break;
		break;

	case PW_TYPE_DATE:
		{
			/*
			 *	time_t may be 64 bits, whule vp_date
			 *	MUST be 32-bits.  We need an
			 *	intermediary variable to handle
			 *	the conversions.
			 */
			time_t date;

			if (gettime(value, &date) < 0) {
				fr_strerror_printf("failed to parse time string "
					   "\"%s\"", value);
				return NULL;
			}

			vp->vp_date = date;
		}
		vp->length = 4;
		break;

	case PW_TYPE_ABINARY:
#ifdef WITH_ASCEND_BINARY
		if (strncasecmp(value, "0x", 2) == 0) {
			goto do_octets;
		}

		if (ascend_parse_filter(vp) < 0 ) {
			char buffer[256];

			snprintf(buffer, sizeof(buffer), "failed to parse Ascend binary attribute: %s", fr_strerror());
			fr_strerror_printf("%s", buffer);
			return NULL;
		}
		break;

		/*
		 *	If Ascend binary is NOT defined,
		 *	then fall through to raw octets, so that
		 *	the user can at least make them by hand...
		 */
	do_octets:
#endif
	/* raw octets: 0x01020304... */
	case PW_TYPE_OCTETS:
		if (strncasecmp(value, "0x", 2) == 0) {
			size_t size;
			uint8_t *us;

			cp = value + 2;
			us = vp->vp_octets;
			vp->length = 0;

			/*
			 *	Invalid.
			 */
			size = strlen(cp);
			if ((size  & 0x01) != 0) {
				fr_strerror_printf("Hex string is not an even length string.");
				return NULL;
			}

			vp->length = size >> 1;
			if (size > 2*sizeof(vp->vp_octets)) {
				us = vp->vp_tlv = malloc(vp->length);
				if (!us) {
					fr_strerror_printf("Out of memory.");
					return NULL;
				}
			}

			if (fr_hex2bin(cp, us, vp->length) != vp->length) {
				fr_strerror_printf("Invalid hex data");
				return NULL;
			}
		}
		break;

	case PW_TYPE_IFID:
		if (ifid_aton(value, (void *) &vp->vp_ifid) == NULL) {
			fr_strerror_printf("failed to parse interface-id "
				   "string \"%s\"", value);
			return NULL;
		}
		vp->length = 8;
		break;

	case PW_TYPE_IPV6ADDR:
		{
			fr_ipaddr_t ipaddr;

			if (ip_hton(value, AF_INET6, &ipaddr) < 0) {
				char buffer[1024];

				strlcpy(buffer, fr_strerror(), sizeof(buffer));

				fr_strerror_printf("failed to parse IPv6 address "
						   "string \"%s\": %s", value, buffer);
				return NULL;
			}
			vp->vp_ipv6addr = ipaddr.ipaddr.ip6addr;
			vp->length = 16; /* length of IPv6 address */
		}
		break;

	case PW_TYPE_IPV6PREFIX:
		p = strchr(value, '/');
		if (!p || ((p - value) >= 256)) {
			fr_strerror_printf("invalid IPv6 prefix "
				   "string \"%s\"", value);
			return NULL;
		} else {
			unsigned int prefix;
			char buffer[256], *eptr;

			memcpy(buffer, value, p - value);
			buffer[p - value] = '\0';

			if (inet_pton(AF_INET6, buffer, vp->vp_octets + 2) <= 0) {
				fr_strerror_printf("failed to parse IPv6 address "
					   "string \"%s\"", value);
				return NULL;
			}

			prefix = strtoul(p + 1, &eptr, 10);
			if ((prefix > 128) || *eptr) {
				fr_strerror_printf("failed to parse IPv6 address "
					   "string \"%s\"", value);
				return NULL;
			}
			vp->vp_octets[1] = prefix;
		}
		vp->vp_octets[0] = 0;
		vp->length = 16 + 2;
		break;

	case PW_TYPE_IPV4PREFIX:
		p = strchr(value, '/');
		if (!p || ((p - value) >= 256)) {
			fr_strerror_printf("invalid IPv4 prefix "
				   "string \"%s\"", value);
			return NULL;
		} else {
			unsigned int prefix;
			char buffer[256], *eptr;

			memcpy(buffer, value, p - value);
			buffer[p - value] = '\0';

			if (inet_pton(AF_INET, buffer, vp->vp_octets + 2) <= 0) {
				fr_strerror_printf("failed to parse IPv6 address "
					   "string \"%s\"", value);
				return NULL;
			}

			prefix = strtoul(p + 1, &eptr, 10);
			if ((prefix > 32) || *eptr) {
				fr_strerror_printf("failed to parse IPv6 address "
					   "string \"%s\"", value);
				return NULL;
			}
			vp->vp_octets[1] = prefix;

			if (prefix < 32) {
				uint32_t addr, mask;

				memcpy(&addr, vp->vp_octets + 2, sizeof(addr));
				mask = 1;
				mask <<= (32 - prefix);
				mask--;
				mask = ~mask;
				mask = htonl(mask);
				addr &= mask;
				memcpy(vp->vp_octets + 2, &addr, sizeof(addr));
			}
		}
		vp->vp_octets[0] = 0;
		vp->length = sizeof(vp->vp_ipv4prefix);
		break;

	case PW_TYPE_ETHERNET:
		{
			const char *c1, *c2;

			length = 0;
			cp = value;
			while (*cp) {
				if (cp[1] == ':') {
					c1 = hextab;
					c2 = memchr(hextab, tolower((int) cp[0]), 16);
					cp += 2;
				} else if ((cp[1] != '\0') &&
					   ((cp[2] == ':') ||
					    (cp[2] == '\0'))) {
					   c1 = memchr(hextab, tolower((int) cp[0]), 16);
					   c2 = memchr(hextab, tolower((int) cp[1]), 16);
					   cp += 2;
					   if (*cp == ':') cp++;
				} else {
					c1 = c2 = NULL;
				}
				if (!c1 || !c2 || (length >= sizeof(vp->vp_ether))) {
					fr_strerror_printf("failed to parse Ethernet address \"%s\"", value);
					return NULL;
				}
				vp->vp_ether[length] = ((c1-hextab)<<4) + (c2-hextab);
				length++;
			}
		}
		vp->length = 6;
		break;
		
	/* 
	 *	Crazy polymorphic (IPv4/IPv6) attribute type for WiMAX.
	 *
	 *	We try and make is saner by replacing the original
	 *	da, with either an IPv4 or IPv6 da type.
	 *
	 *	These are not dynamic da, and will have the same vendor
	 *	and attribute as the original.
	 */
	case PW_TYPE_COMBO_IP:
		{
			const DICT_ATTR *da;
		
			if (inet_pton(AF_INET6, value, vp->vp_strvalue) > 0) {
				da = dict_attrbytype(vp->da->attr, vp->da->vendor,
						     PW_TYPE_IPV6ADDR);
				if (!da) {
					return NULL;
				}
			
				vp->length = 16; /* length of IPv6 address */
				vp->vp_strvalue[vp->length] = '\0';
			} else {
				fr_ipaddr_t ipaddr;
			
				da = dict_attrbytype(vp->da->attr, vp->da->vendor,
						     PW_TYPE_IPADDR);
				if (!da) {
					return NULL;
				}

				if (ip_hton(value, AF_INET, &ipaddr) < 0) {
					fr_strerror_printf("Failed to find IPv4 address for %s", value);
					return NULL;
				}

				vp->vp_ipaddr = ipaddr.ipaddr.ip4addr.s_addr;
				vp->length = 4;
			}
		
			vp->da = da;
		}
		
		break;

	case PW_TYPE_SIGNED: /* Damned code for 1 WiMAX attribute */
		vp->vp_signed = (int32_t) strtol(value, &p, 10);
		vp->length = 4;
		break;

	case PW_TYPE_TLV: /* don't use this! */
		if (strncasecmp(value, "0x", 2) != 0) {
			fr_strerror_printf("Invalid TLV specification");
			return NULL;
		}
		length = strlen(value + 2) / 2;
		if (vp->length < length) {
			free(vp->vp_tlv);
			vp->vp_tlv = NULL;
		}
		vp->vp_tlv = malloc(length);
		if (!vp->vp_tlv) {
			fr_strerror_printf("No memory");
			return NULL;
		}
		if (fr_hex2bin(value + 2, vp->vp_tlv,
			       length) != length) {
			fr_strerror_printf("Invalid hex data in TLV");
			return NULL;
		}
		vp->length = length;
		break;

		/*
		 *  Anything else.
		 */
	default:
		fr_strerror_printf("unknown attribute type %d", vp->da->type);
		return NULL;
	}

	return vp;
}

/** Create a valuepair from an ASCII attribute and value
 *
 * Where the attribute name is in the form:
 *  - Attr-%d
 *  - Attr-%d.%d.%d...
 *  - Vendor-%d-Attr-%d
 *  - VendorName-Attr-%d
 *
 * @param attribute name to parse.
 * @param value to parse (must be a hex string).
 * @param op to assign to new valuepair.
 * @return new valuepair or NULL on error.
 */
static VALUE_PAIR *pairmake_any(const char *attribute, const char *value,
				FR_TOKEN op)
{
	VALUE_PAIR	*vp;
	const DICT_ATTR *da;
	
	uint8_t		*data;
	size_t		size;

	/*
	 *	Unknown attributes MUST be of type 'octets'
	 */
	if (value && (strncasecmp(value, "0x", 2) != 0)) {
		fr_strerror_printf("Unknown attribute \"%s\" requires a hex "
				   "string, not \"%s\"", attribute, value);
		return NULL;
	}

	da = dict_attrunknownbyname(attribute);
	if (!da) {
		return NULL;
	}

	/*
	 *	We've now parsed the attribute properly, Let's create
	 *	it.  This next stop also looks the attribute up in the
	 *	dictionary, and creates the appropriate type for it.
	 */
	vp = pairalloc(da);
	if (!vp) {
		return NULL;
	}

	vp->op = (op == 0) ? T_OP_EQ : op;
	
	if (!value) return vp;

	size = strlen(value + 2);
	data = vp->vp_octets;

	vp->length = size >> 1;
	if (vp->length > sizeof(vp->vp_octets)) {
		vp->vp_tlv = malloc(vp->length);
		if (!vp->vp_tlv) {
			fr_strerror_printf("Out of memory");
			free(vp);
			return NULL;
		}
		data = vp->vp_tlv;
	}

	if (fr_hex2bin(value + 2, data, size) != vp->length) {
		fr_strerror_printf("Invalid hex string");
		free(vp);
		return NULL;
	}

	return vp;
}


/*
 *	Create a VALUE_PAIR from an ASCII attribute and value.
 */
VALUE_PAIR *pairmake(const char *attribute, const char *value, FR_TOKEN op)
{
	const DICT_ATTR	*da;
	VALUE_PAIR	*vp;
	char            *tc, *ts;
	int8_t		tag;
	int             found_tag;
	char		buffer[256];
	const char	*attrname = attribute;

	/*
	 *    Check for tags in 'Attribute:Tag' format.
	 */
	found_tag = 0;
	tag = 0;

	ts = strrchr(attribute, ':');
	if (ts && !ts[1]) {
		fr_strerror_printf("Invalid tag for attribute %s", attribute);
		return NULL;
	}

	if (ts && ts[1]) {
		strlcpy(buffer, attribute, sizeof(buffer));
		attrname = buffer;
		ts = strrchr(attrname, ':');

	         /* Colon found with something behind it */
	         if (ts[1] == '*' && ts[2] == 0) {
		         /* Wildcard tag for check items */
		         tag = TAG_ANY;
			 *ts = 0;
		 } else if ((ts[1] >= '0') && (ts[1] <= '9')) {
		         /* It's not a wild card tag */
		         tag = strtol(ts + 1, &tc, 0);
			 if (tc && !*tc && TAG_VALID_ZERO(tag))
				 *ts = 0;
			 else tag = 0;
		 } else {
			 fr_strerror_printf("Invalid tag for attribute %s", attribute);
			 return NULL;
		 }
		 found_tag = 1;
	}

	/*
	 *	It's not found in the dictionary, so we use
	 *	another method to create the attribute.
	 */
	if ((da = dict_attrbyname(attrname)) == NULL) {
		return pairmake_any(attrname, value, op);
	}

	if ((vp = pairalloc(da)) == NULL) {
		return NULL;
	}

	vp->op = (op == 0) ? T_OP_EQ : op;

	/*      Check for a tag in the 'Merit' format of:
	 *      :Tag:Value.  Print an error if we already found
	 *      a tag in the Attribute.
	 */

	if (value && (*value == ':' && da->flags.has_tag)) {
	        /* If we already found a tag, this is invalid */
	        if(found_tag) {
			fr_strerror_printf("Duplicate tag %s for attribute %s",
				   value, vp->da->name);
			DEBUG("Duplicate tag %s for attribute %s\n",
				   value, vp->da->name);
		        pairbasicfree(vp);
			return NULL;
		}
	        /* Colon found and attribute allows a tag */
	        if (value[1] == '*' && value[2] == ':') {
		       /* Wildcard tag for check items */
		       tag = TAG_ANY;
		       value += 3;
		} else {
	               /* Real tag */
		       tag = strtol(value + 1, &tc, 0);
		       if (tc && *tc==':' && TAG_VALID_ZERO(tag))
			    value = tc + 1;
		       else tag = 0;
		}
		found_tag = 1;
	}

	if (found_tag) {
		vp->tag = tag;
	}

	switch (vp->op) {
	default:
		break;

		/*
		 *      For =* and !* operators, the value is irrelevant
		 *      so we return now.
		 */
	case T_OP_CMP_TRUE:
	case T_OP_CMP_FALSE:
		vp->vp_strvalue[0] = '\0';
		vp->length = 0;
	        return vp;
		break;

		/*
		 *	Regular expression comparison of integer attributes
		 *	does a STRING comparison of the names of their
		 *	integer attributes.
		 */
	case T_OP_REG_EQ:	/* =~ */
	case T_OP_REG_NE:	/* !~ */
#ifndef WITH_REGEX
		fr_strerror_printf("Regular expressions are not supported");
		return NULL;

#else
		if (!value) {
			/* just return the vp - we've probably been called
			 * by pairmake_xlat who will fill in the value for us
			 */
			return vp;
		}

		pairbasicfree(vp);
		
		if (1) {
			int compare;
			regex_t reg;
			
			compare = regcomp(&reg, value, REG_EXTENDED);
			if (compare != 0) {
				regerror(compare, &reg, buffer, sizeof(buffer));
				fr_strerror_printf("Illegal regular expression in attribute: %s: %s",
					   attribute, buffer);
				return NULL;
			}
		}

		return pairmake_xlat(attribute, value, op);
#endif
	}

	/*
	 *	FIXME: if (strcasecmp(attribute, vp->da->name) != 0)
	 *	then the user MAY have typed in the attribute name
	 *	as Vendor-%d-Attr-%d, and the value MAY be octets.
	 *
	 *	We probably want to fix pairparsevalue to accept
	 *	octets as values for any attribute.
	 */
	if (value && (pairparsevalue(vp, value) == NULL)) {
		pairbasicfree(vp);
		return NULL;
	}

	return vp;
}

/** Mark a valuepair for xlat expansion
 *
 * Copies xlat source (unprocessed) string to valuepair value,
 * and sets value type.
 *
 * @param vp to mark for expansion.
 * @param value to expand.
 * @return 0 if marking succeeded or -1 if vp already had a value, or OOM.
 */
int pairmark_xlat(VALUE_PAIR *vp, const char *value)
{
	char *xlat;
	
	/*
	 *	valuepair should not already have a value.
	 */
	if (vp->vt != VT_NONE) {
		return -1;
	}
	
	xlat = strdup(value);
	if (!xlat) {
		return -1;
	}
	
	vp->vt = VT_XLAT;
	vp->value.xlat = xlat;

	return 0;	 
}

/** Create a valuepair and mark it for xlat expansion
 *
 * The valuepair xlat string will be expanded, parsed and then freed when
 * the valuepair is added into a valuepair tree.
 *
 * @param attribute to use to create the valuepair.
 * @param value to expand.
 * @param op to assign to new valuepair.
 * @return valuepair or NULL on error.
 */
VALUE_PAIR *pairmake_xlat(const char *attribute, const char *value,
			  FR_TOKEN op)
{
	VALUE_PAIR *vp;

	if (!value) {
		fr_strerror_printf("Empty value passed to pairmake_xlat()");
		return NULL;
	}

	vp = pairmake(attribute, NULL, op);
	if (!vp) {
		return vp;
	}
	
	if (pairmark_xlat(vp, value) < 0) {
		pairbasicfree(vp);
		
		return NULL; 
	}

	vp->length = 0;

	return vp;
}

/*
 *	[a-zA-Z0-9_-:.]+
 */
static const int valid_attr_name[256] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,
	0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1,
	0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/*
 *	Read a valuepair from a buffer, and advance pointer.
 *	Sets *eol to T_EOL if end of line was encountered.
 */
VALUE_PAIR *pairread(const char **ptr, FR_TOKEN *eol)
{
	char		buf[64];
	char		attr[64];
	char		value[1024], *q;
	const char	*p;
	FR_TOKEN	token, t, xlat;
	VALUE_PAIR	*vp;
	size_t		len;

	*eol = T_OP_INVALID;

	p = *ptr;
	while ((*p == ' ') || (*p == '\t')) p++;

	if (!*p) {
		*eol = T_OP_INVALID;
		fr_strerror_printf("No token read where we expected an attribute name");
		return NULL;
	}

	if (*p == '#') {
		*eol = T_HASH;
		fr_strerror_printf("Read a comment instead of a token");
		return NULL;
	}

	q = attr;
	for (len = 0; len < sizeof(attr); len++) {
		if (valid_attr_name[(int)*p]) {
			*q++ = *p++;
			continue;
		}
		break;
	}

	if (len == sizeof(attr)) {
		*eol = T_OP_INVALID;
		fr_strerror_printf("Attribute name is too long");
		return NULL;
	}

	/*
	 *	We may have Foo-Bar:= stuff, so back up.
	 */
	if ((len > 0) && (attr[len - 1] == ':')) {
		p--;
		len--;
	}

	attr[len] = '\0';
	*ptr = p;

	/* Now we should have an operator here. */
	token = gettoken(ptr, buf, sizeof(buf));
	if (token < T_EQSTART || token > T_EQEND) {
		fr_strerror_printf("expecting operator");
		return NULL;
	}

	/* Read value.  Note that empty string values are allowed */
	xlat = gettoken(ptr, value, sizeof(value));
	if (xlat == T_EOL) {
		fr_strerror_printf("failed to get value");
		return NULL;
	}

	/*
	 *	Peek at the next token. Must be T_EOL, T_COMMA, or T_HASH
	 */
	p = *ptr;
	t = gettoken(&p, buf, sizeof(buf));
	if (t != T_EOL && t != T_COMMA && t != T_HASH) {
		fr_strerror_printf("Expected end of line or comma");
		return NULL;
	}

	*eol = t;
	if (t == T_COMMA) {
		*ptr = p;
	}

	vp = NULL;
	switch (xlat) {
		/*
		 *	Make the full pair now.
		 */
	default:
		vp = pairmake(attr, value, token);
		break;

		/*
		 *	Perhaps do xlat's
		 */
	case T_DOUBLE_QUOTED_STRING:
		p = strchr(value, '%');
		if (p && (p[1] == '{')) {
			if (strlen(value) >= sizeof(vp->vp_strvalue)) {
				fr_strerror_printf("Value too long");
				return NULL;
			}
			vp = pairmake_xlat(attr, value, token);
			if (!vp) {
				*eol = T_OP_INVALID;
				return NULL;
			}

		} else {
			/*
			 *	Parse && escape it, as defined by the
			 *	data type.
			 */
			vp = pairmake(attr, value, token);
			if (!vp) {
				*eol = T_OP_INVALID;
				return NULL;
			}
		}
		break;

	case T_SINGLE_QUOTED_STRING:
		vp = pairmake(attr, NULL, token);
		if (!vp) {
			*eol = T_OP_INVALID;
			return NULL;
		}

		/*
		 *	String and octet types get copied verbatim.
		 */
		if ((vp->da->type == PW_TYPE_STRING) ||
		    (vp->da->type == PW_TYPE_OCTETS)) {
			strlcpy(vp->vp_strvalue, value,
				sizeof(vp->vp_strvalue));
			vp->length = strlen(vp->vp_strvalue);

			/*
			 *	Everything else gets parsed: it's
			 *	DATA, not a string!
			 */
		} else if (!pairparsevalue(vp, value)) {
			pairfree(&vp);
			*eol = T_OP_INVALID;
			return NULL;
		}
		break;

		/*
		 *	Mark the pair to be allocated later.
		 */
	case T_BACK_QUOTED_STRING:
		if (strlen(value) >= sizeof(vp->vp_strvalue)) {
			fr_strerror_printf("Value too long");
			return NULL;
		}

		vp = pairmake_xlat(attr, value, token);
		if (!vp) {
			*eol = T_OP_INVALID;
			return NULL;
		}
		break;
	}

	/*
	 *	If we didn't make a pair, return an error.
	 */
	if (!vp) {
		*eol = T_OP_INVALID;
		return NULL;
	}

	return vp;
}

/*
 *	Read one line of attribute/value pairs. This might contain
 *	multiple pairs seperated by comma's.
 */
FR_TOKEN userparse(const char *buffer, VALUE_PAIR **first_pair)
{
	VALUE_PAIR	*vp;
	const char	*p;
	FR_TOKEN	last_token = T_OP_INVALID;
	FR_TOKEN	previous_token;

	/*
	 *	We allow an empty line.
	 */
	if (buffer[0] == 0)
		return T_EOL;

	p = buffer;
	do {
		previous_token = last_token;
		if ((vp = pairread(&p, &last_token)) == NULL) {
			return last_token;
		}
		pairadd(first_pair, vp);
	} while (*p && (last_token == T_COMMA));

	/*
	 *	Don't tell the caller that there was a comment.
	 */
	if (last_token == T_HASH) {
		return previous_token;
	}

	/*
	 *	And return the last token which we read.
	 */
	return last_token;
}

/*
 *	Read valuepairs from the fp up to End-Of-File.
 *
 *	Hmm... this function is only used by radclient..
 */
VALUE_PAIR *readvp2(FILE *fp, int *pfiledone, const char *errprefix)
{
	char buf[8192];
	FR_TOKEN last_token = T_EOL;
	VALUE_PAIR *vp;
	VALUE_PAIR *list;
	int error = 0;

	list = NULL;

	while (!error && fgets(buf, sizeof(buf), fp) != NULL) {
		/*
		 *      If we get a '\n' by itself, we assume that's
		 *      the end of that VP
		 */
		if ((buf[0] == '\n') && (list)) {
			return list;
		}
		if ((buf[0] == '\n') && (!list)) {
			continue;
		}

		/*
		 *	Comments get ignored
		 */
		if (buf[0] == '#') continue;

		/*
		 *	Read all of the attributes on the current line.
		 */
		vp = NULL;
		last_token = userparse(buf, &vp);
		if (!vp) {
			if (last_token != T_EOL) {
				fr_perror("%s", errprefix);
				error = 1;
				break;
			}
			break;
		}

		pairadd(&list, vp);
		buf[0] = '\0';
	}

	if (error) pairfree(&list);

	*pfiledone = 1;

	return error ? NULL: list;
}

/*
 *	Compare two pairs, using the operator from "one".
 *
 *	i.e. given two attributes, it does:
 *
 *	(two->data) (one->operator) (one->data)
 *
 *	e.g. "foo" != "bar"
 *
 *	Returns true (comparison is true), or false (comparison is not true);
 */
int paircmp(VALUE_PAIR *one, VALUE_PAIR *two)
{
	int compare;

	switch (one->op) {
	case T_OP_CMP_TRUE:
		return (two != NULL);

	case T_OP_CMP_FALSE:
		return (two == NULL);

		/*
		 *	One is a regex, compile it, print two to a string,
		 *	and then do string comparisons.
		 */
	case T_OP_REG_EQ:
	case T_OP_REG_NE:
#ifndef WITH_REGEX
		return -1;
#else
		{
			regex_t reg;
			char buffer[MAX_STRING_LEN * 4 + 1];

			compare = regcomp(&reg, one->vp_strvalue,
					  REG_EXTENDED);
			if (compare != 0) {
				regerror(compare, &reg, buffer, sizeof(buffer));
				fr_strerror_printf("Illegal regular expression in attribute: %s: %s",
					   one->da->name, buffer);
				return -1;
			}

			vp_prints_value(buffer, sizeof(buffer), two, 0);

			/*
			 *	Don't care about substring matches,
			 *	oh well...
			 */
			compare = regexec(&reg, buffer, 0, NULL, 0);

			regfree(&reg);
			if (one->op == T_OP_REG_EQ) return (compare == 0);
			return (compare != 0);
		}
#endif

	default:		/* we're OK */
		break;
	}

	/*
	 *	Can't compare two attributes of differing types
	 */
	if (one->da->type != two->da->type) {
		return one->da->type - two->da->type;
	}
	
	/*
	 *	After doing the previous check for special comparisons,
	 *	do the per-type comparison here.
	 */
	switch (one->da->type) {
	case PW_TYPE_ABINARY:
	case PW_TYPE_OCTETS:
	{
		size_t length;

		if (one->length < two->length) {
			length = one->length;
		} else {
			length = two->length;
		}

		if (length) {
			compare = memcmp(two->vp_octets, one->vp_octets,
					 length);
			if (compare != 0) break;
		}

		/*
		 *	Contents are the same.  The return code
		 *	is therefore the difference in lengths.
		 *
		 *	i.e. "0x00" is smaller than "0x0000"
		 */
		compare = two->length - one->length;
	}
		break;

	case PW_TYPE_STRING:
		compare = strcmp(two->vp_strvalue, one->vp_strvalue);
		break;

	case PW_TYPE_BYTE:
	case PW_TYPE_SHORT:
	case PW_TYPE_INTEGER:
	case PW_TYPE_DATE:
		compare = two->vp_integer - one->vp_integer;
		break;

	case PW_TYPE_INTEGER64:
		/*
		 *	Don't want integer overflow!
		 */
		if (two->vp_integer64 < one->vp_integer64) {
			compare = -1;
		} else if (two->vp_integer64 > one->vp_integer64) {
			compare = +1;
		} else {
			compare = 0;
		}
		break;
	case PW_TYPE_IPADDR:
		compare = ntohl(two->vp_ipaddr) - ntohl(one->vp_ipaddr);
		break;

	case PW_TYPE_IPV6ADDR:
		compare = memcmp(&two->vp_ipv6addr, &one->vp_ipv6addr,
				 sizeof(two->vp_ipv6addr));
		break;

	case PW_TYPE_IPV6PREFIX:
		compare = memcmp(&two->vp_ipv6prefix, &one->vp_ipv6prefix,
				 sizeof(two->vp_ipv6prefix));
		break;

		/*
		 *	FIXME: do a smarter comparison...
		 */
	case PW_TYPE_IPV4PREFIX:
		compare = memcmp(&two->vp_ipv4prefix, &one->vp_ipv4prefix,
				 sizeof(two->vp_ipv4prefix));
		break;

	case PW_TYPE_IFID:
		compare = memcmp(&two->vp_ifid, &one->vp_ifid,
				 sizeof(two->vp_ifid));
		break;

	default:
		return 0;	/* unknown type */
	}

	/*
	 *	Now do the operator comparison.
	 */
	switch (one->op) {
	case T_OP_CMP_EQ:
		return (compare == 0);

	case T_OP_NE:
		return (compare != 0);

	case T_OP_LT:
		return (compare < 0);

	case T_OP_GT:
		return (compare > 0);

	case T_OP_LE:
		return (compare <= 0);

	case T_OP_GE:
		return (compare >= 0);

	default:
		return 0;
	}

	return 0;
}
