/*
 * Copyright (C) 2008 Search Solution Corporation. All rights reserved by Search Solution.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

/*
 * language_support.c : Multi-language and character set support
 */

#ident "$Id$"

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#if !defined(WINDOWS)
#include <langinfo.h>
#endif

#include "chartype.h"
#include "misc_string.h"
#include "language_support.h"
#include "authenticate.h"
#include "environment_variable.h"
#include "db.h"
#if !defined(WINDOWS)
#include <dlfcn.h>
#endif

/* this must be the last header file included! */
#include "dbval.h"

#define SYMBOL_NAME_SIZE 128

static INTL_LANG lang_Lang_id = INTL_LANG_ENGLISH;
static INTL_CODESET lang_Loc_charset = INTL_CODESET_ISO88591;
static char lang_Loc_name[LANG_MAX_LANGNAME] = LANG_NAME_DEFAULT;
static char lang_user_Loc_name[LANG_MAX_LANGNAME];
static char lang_msg_Loc_name[LANG_MAX_LANGNAME] = LANG_NAME_DEFAULT;
static char lang_Lang_name[LANG_MAX_LANGNAME] = LANG_NAME_DEFAULT;
static DB_CURRENCY lang_Loc_currency = DB_CURRENCY_DOLLAR;

/* locale data */
static LANG_LOCALE_DATA lc_English_iso88591;
static LANG_LOCALE_DATA lc_English_utf8;
static LANG_LOCALE_DATA lc_Turkish_iso88591;
static LANG_LOCALE_DATA lc_Turkish_utf8;
static LANG_LOCALE_DATA lc_Korean_iso88591;
static LANG_LOCALE_DATA lc_Korean_utf8;
static LANG_LOCALE_DATA lc_Korean_euckr;
static LANG_LOCALE_DATA *lang_Loc_data = &lc_English_iso88591;

static bool lang_Initialized = false;
static bool lang_Fully_Initialized = false;
static bool lang_Init_w_error = false;
static bool lang_env_initialized = false;

typedef struct lang_defaults LANG_DEFAULTS;
struct lang_defaults
{
  const char *lang_name;
  const INTL_LANG lang;
  const INTL_CODESET codeset;
};

/* Order of language/charset pair is important: first encoutered charset is
 * the default for a language */
LANG_DEFAULTS builtin_langs[] = {
  /* English - ISO-8859-1 - default lang and charset */
  {LANG_NAME_ENGLISH, INTL_LANG_ENGLISH, INTL_CODESET_ISO88591},
  /* English - UTF-8 */
  {LANG_NAME_ENGLISH, INTL_LANG_ENGLISH, INTL_CODESET_UTF8},
  /* Korean - UTF-8 */
  {LANG_NAME_KOREAN, INTL_LANG_KOREAN, INTL_CODESET_UTF8},
  /* Korean - EUC-KR */
  {LANG_NAME_KOREAN, INTL_LANG_KOREAN, INTL_CODESET_KSC5601_EUC},
  /* Korean - ISO-8859-1 : contains romanized names for months, days */
  {LANG_NAME_KOREAN, INTL_LANG_KOREAN, INTL_CODESET_ISO88591},
  /* Turkish - UTF-8 */
  {LANG_NAME_TURKISH, INTL_LANG_TURKISH, INTL_CODESET_UTF8},
  /* Turkish - ISO-8859-1 : contains romanized names for months, days */
  {LANG_NAME_TURKISH, INTL_LANG_TURKISH, INTL_CODESET_ISO88591}
};

static void **loclib_handle = NULL;
static int loclib_handle_size = 0;
static int loclib_handle_count = 0;

static TEXT_CONVERSION *console_conv = NULL;
extern TEXT_CONVERSION con_iso_8859_1_conv;
extern TEXT_CONVERSION con_iso_8859_9_conv;

/* all loaded locales */
static LANG_LOCALE_DATA *lang_loaded_locales[LANG_MAX_LOADED_LOCALES] =
  { NULL };

static int lang_count_locales = 0;

/* all loaded collations */
static LANG_COLLATION *lang_collations[LANG_MAX_COLLATIONS] = { NULL };

static int lang_count_collations = 0;

/* normalization data */
static UNICODE_NORMALIZATION *generic_unicode_norm = NULL;

static const DB_CHARSET lang_Db_charsets[] = {
  {"ascii", "US English charset - ASCII encoding", " ", "",
   "", INTL_CODESET_ASCII, 1},
  {"raw-bits", "Uninterpreted bits - Raw encoding", "", "",
   "", INTL_CODESET_RAW_BITS, 1},
  {"raw-bytes", "Uninterpreted bytes - Raw encoding", "", "",
   "", INTL_CODESET_RAW_BYTES, 1},
  {"iso8859-1", "Latin 1 charset - ISO 8859 encoding", " ", "_iso88591",
   "iso88591", INTL_CODESET_ISO88591, 1},
  {"ksc-euc", "KSC 5601 1990 charset - EUC encoding", "\241\241", "_euckr",
   "euckr", INTL_CODESET_KSC5601_EUC, 2},
  {"utf-8", "UNICODE charset - UTF-8 encoding", " ", "_utf8",
   "utf8", INTL_CODESET_UTF8, 1},
  {"", "", "", "", "", INTL_CODESET_NONE, 0}
};

static void set_current_locale (bool is_full_init);
static int set_lang_from_env (void);
static int check_env_lang_val (char *env_val, char *lang_name,
			       char **charset_ptr, INTL_CODESET * codeset);
static void set_default_lang (void);
static void lang_unload_libraries (void);
static void destroy_user_locales (void);
static int init_user_locales (void);
static LANG_LOCALE_DATA *find_lang_locale_data (const char *name,
						const INTL_CODESET codeset,
						LANG_LOCALE_DATA **
						last_lang_locale);
static int register_lang_locale_data (LANG_LOCALE_DATA * lld);
static void free_lang_locale_data (LANG_LOCALE_DATA * lld);
static int register_collation (LANG_COLLATION * coll);

static bool lang_is_codeset_allowed (const INTL_LANG intl_id,
				     const INTL_CODESET codeset);
static int lang_get_builtin_lang_id_from_name (const char *lang_name,
					       INTL_LANG * lang_id);
static INTL_CODESET lang_get_default_codeset (const INTL_LANG intl_id);

static int lang_fastcmp_iso_88591 (const LANG_COLLATION * lang_coll,
				   const unsigned char *string1,
				   const int size1,
				   const unsigned char *string2,
				   const int size2);
static int lang_strmatch_iso_88591 (const LANG_COLLATION * lang_coll,
				    bool is_match,
				    const unsigned char *str1,
				    int size1,
				    const unsigned char *str2,
				    int size2,
				    const unsigned char *escape,
				    const bool has_last_escape,
				    int *str1_match_size);
static int lang_fastcmp_byte (const LANG_COLLATION * lang_coll,
			      const unsigned char *string1,
			      const int size1,
			      const unsigned char *string2, const int size2);
static int lang_next_alpha_char_iso88591 (const LANG_COLLATION * lang_coll,
					  const unsigned char *seq,
					  const int size,
					  unsigned char *next_seq,
					  int *len_next);
static int lang_next_coll_byte (const LANG_COLLATION * lang_coll,
				const unsigned char *seq, const int size,
				unsigned char *next_seq, int *len_next);
static int lang_strcmp_utf8 (const LANG_COLLATION * lang_coll,
			     const unsigned char *str1, const int size1,
			     const unsigned char *str2, const int size2);
static int lang_strmatch_utf8 (const LANG_COLLATION * lang_coll,
			       bool is_match,
			       const unsigned char *str1, int size1,
			       const unsigned char *str2, int size2,
			       const unsigned char *escape,
			       const bool has_last_escape,
			       int *str1_match_size);
static int lang_strcmp_utf8_w_contr (const LANG_COLLATION * lang_coll,
				     const unsigned char *str1,
				     const int size1,
				     const unsigned char *str2,
				     const int size2);
static unsigned int lang_get_w_first_el (const COLL_DATA * coll,
					 const unsigned char *str,
					 const int str_size,
					 unsigned char **next_char);
static int lang_strmatch_utf8_w_contr (const LANG_COLLATION * lang_coll,
				       bool is_match,
				       const unsigned char *str1, int size1,
				       const unsigned char *str2, int size2,
				       const unsigned char *escape,
				       const bool has_last_escape,
				       int *str1_match_size);
static COLL_CONTRACTION *lang_get_contr_for_string (const COLL_DATA *
						    coll_data,
						    const unsigned char *str,
						    const int str_size,
						    unsigned int cp);
static void lang_get_uca_w_l13 (const COLL_DATA * coll_data,
				const bool use_contractions,
				const unsigned char *str, const int size,
				UCA_L13_W ** uca_w_l13, int *num_ce,
				unsigned char **str_next,
				unsigned int *cp_out);
static void lang_get_uca_back_weight_l13 (const COLL_DATA * coll_data,
					  const bool use_contractions,
					  const unsigned char *str_start,
					  const unsigned char *str_last,
					  UCA_L13_W ** uca_w_l13, int *num_ce,
					  unsigned char **str_prev,
					  unsigned int *cp_out);
static void lang_get_uca_w_l4 (const COLL_DATA * coll_data,
			       const bool use_contractions,
			       const unsigned char *str, const int size,
			       UCA_L4_W ** uca_w_l4, int *num_ce,
			       unsigned char **str_next,
			       unsigned int *cp_out);
static int lang_strmatch_utf8_uca_w_level (const COLL_DATA * coll_data,
					   const int level, bool is_match,
					   const unsigned char *str1,
					   const int size1,
					   const unsigned char *str2,
					   const int size2,
					   const unsigned char *escape,
					   const bool has_last_escape,
					   int *offset_next_level,
					   int *str1_match_size);
static int lang_back_strmatch_utf8_uca_w_level (const COLL_DATA * coll_data,
						bool is_match,
						const unsigned char *str1,
						const int size1,
						const unsigned char *str2,
						const int size2,
						const unsigned char *escape,
						const bool has_last_escape,
						int *offset_next_level,
						int *str1_match_size);
static int lang_strcmp_utf8_uca (const LANG_COLLATION * lang_coll,
				 const unsigned char *str1, const int size1,
				 const unsigned char *str2, const int size2);
static int lang_strmatch_utf8_uca (const LANG_COLLATION * lang_coll,
				   bool is_match, const unsigned char *str1,
				   const int size1, const unsigned char *str2,
				   const int size2,
				   const unsigned char *escape,
				   const bool has_last_escape,
				   int *str1_match_size);
static int lang_strcmp_check_trail_spaces (const unsigned char *str1,
					   const int size1,
					   const unsigned char *str2,
					   const int size2);
static int lang_next_coll_char_utf8 (const LANG_COLLATION * lang_coll,
				     const unsigned char *seq, const int size,
				     unsigned char *next_seq, int *len_next);
static int lang_next_coll_seq_utf8_w_contr (const LANG_COLLATION * lang_coll,
					    const unsigned char *seq,
					    const int size,
					    unsigned char *next_seq,
					    int *len_next);
static int lang_split_key_iso (const LANG_COLLATION * lang_coll,
			       const bool is_desc,
			       const unsigned char *str1, const int size1,
			       const unsigned char *str2, const int size2,
			       unsigned char **key, int *byte_size);
static int lang_split_key_byte (const LANG_COLLATION * lang_coll,
				const bool is_desc,
				const unsigned char *str1, const int size1,
				const unsigned char *str2, const int size2,
				unsigned char **key, int *byte_size);
static int lang_split_key_utf8 (const LANG_COLLATION * lang_coll,
				const bool is_desc,
				const unsigned char *str1, const int size1,
				const unsigned char *str2, const int size2,
				unsigned char **key, int *byte_size);
static int lang_split_key_w_exp (const LANG_COLLATION * lang_coll,
				 const bool is_desc,
				 const unsigned char *str1, const int size1,
				 const unsigned char *str2, const int size2,
				 unsigned char **key, int *byte_size);
static int lang_split_key_euc (const LANG_COLLATION * lang_coll,
			       const bool is_desc,
			       const unsigned char *str1, const int size1,
			       const unsigned char *str2, const int size2,
			       unsigned char **key, int *byte_size);
static void lang_init_coll_en_ci (LANG_COLLATION * lang_coll);
static void lang_init_coll_utf8_en_cs (LANG_COLLATION * lang_coll);
static void lang_init_coll_utf8_tr_cs (LANG_COLLATION * lang_coll);
static int lang_fastcmp_ko (const LANG_COLLATION * lang_coll,
			    const unsigned char *string1, const int size1,
			    const unsigned char *string2, const int size2);
static int lang_strmatch_ko (const LANG_COLLATION * lang_coll, bool is_match,
			     const unsigned char *str1, int size1,
			     const unsigned char *str2, int size2,
			     const unsigned char *escape,
			     const bool has_last_escape,
			     int *str1_match_size);
static int lang_next_alpha_char_ko (const LANG_COLLATION * lang_coll,
				    const unsigned char *seq, const int size,
				    unsigned char *next_seq, int *len_next);
static int lang_locale_load_alpha_from_lib (ALPHABET_DATA * a,
					    bool load_w_identifier_name,
					    const char *alpha_suffix,
					    void *lib_handle,
					    const LOCALE_FILE * lf);
static int
lang_locale_load_normalization_from_lib (UNICODE_NORMALIZATION * norm,
					 void *lib_handle,
					 const LOCALE_FILE * lf);
static void lang_free_collations (void);

/* built-in collations */
/* number of characters in the (extended) alphabet per language */
#define LANG_CHAR_COUNT_EN 256
#define LANG_CHAR_COUNT_TR 352

#define LANG_COLL_GENERIC_SORT_OPT \
  {TAILOR_UNDEFINED, false, false, 1, false, CONTR_IGNORE, false, \
   MATCH_CONTR_BOUND_ALLOW}
#define LANG_COLL_NO_EXP 0, NULL, NULL, NULL
#define LANG_COLL_NO_CONTR NULL, 0, 0, NULL, 0, 0

#define LANG_NO_NORMALIZATION {NULL, 0, NULL, NULL, 0}

static unsigned int lang_weight_EN_cs[LANG_CHAR_COUNT_EN];
static unsigned int lang_next_alpha_char_EN_cs[LANG_CHAR_COUNT_EN];

static unsigned int lang_weight_EN_ci[LANG_CHAR_COUNT_EN];
static unsigned int lang_next_alpha_char_EN_ci[LANG_CHAR_COUNT_EN];


static unsigned int lang_weight_TR[LANG_CHAR_COUNT_TR];
static unsigned int lang_next_alpha_char_TR[LANG_CHAR_COUNT_TR];

#define DEFAULT_COLL_OPTIONS {true, true, true}
#define CI_COLL_OPTIONS {false, false, true}

static LANG_COLLATION coll_iso_binary = {
  INTL_CODESET_ISO88591, 1, 0, DEFAULT_COLL_OPTIONS, NULL,
  /* collation data */
  {LANG_COLL_ISO_BINARY, "iso88591_bin",
   LANG_COLL_GENERIC_SORT_OPT,
   NULL, NULL, 0,
   LANG_COLL_NO_EXP,
   LANG_COLL_NO_CONTR,
   "54735f231842c3a673161fc90670989b"},
  lang_fastcmp_iso_88591,
  lang_strmatch_iso_88591,
  lang_next_alpha_char_iso88591,
  lang_split_key_iso,
  NULL
};

static LANG_COLLATION coll_utf8_binary = {
  INTL_CODESET_UTF8, 1, 0, DEFAULT_COLL_OPTIONS, NULL,
  /* collation data */
  {LANG_COLL_UTF8_BINARY, "utf8_bin",
   LANG_COLL_GENERIC_SORT_OPT,
   lang_weight_EN_cs, lang_next_alpha_char_EN_cs, LANG_CHAR_COUNT_EN,
   LANG_COLL_NO_EXP,
   LANG_COLL_NO_CONTR,
   "d16a9a3825e263f76028c1e8c3cd043d"},
  /* compare functions handles bytes, no need to handle UTF-8 chars */
  lang_fastcmp_byte,
  lang_strmatch_utf8,
  /* 'next' and 'split_point' functions must handle UTF-8 chars */
  lang_next_coll_char_utf8,
  lang_split_key_utf8,
  NULL
};

static LANG_COLLATION coll_iso88591_en_cs = {
  INTL_CODESET_ISO88591, 1, 0, DEFAULT_COLL_OPTIONS, NULL,
  /* collation data */
  {LANG_COLL_ISO_EN_CS, "iso88591_en_cs",
   LANG_COLL_GENERIC_SORT_OPT,
   NULL, NULL, 0,
   LANG_COLL_NO_EXP,
   LANG_COLL_NO_CONTR,
   "707cef004e58be204d999d8a2abb4cc3"},
  lang_fastcmp_iso_88591,
  lang_strmatch_iso_88591,
  lang_next_alpha_char_iso88591,
  lang_split_key_iso,
  NULL
};

static LANG_COLLATION coll_iso88591_en_ci = {
  INTL_CODESET_ISO88591, 1, 0, CI_COLL_OPTIONS, NULL,
  /* collation data */
  {LANG_COLL_ISO_EN_CI, "iso88591_en_ci",
   LANG_COLL_GENERIC_SORT_OPT,
   lang_weight_EN_ci, lang_next_alpha_char_EN_ci, LANG_CHAR_COUNT_EN,
   LANG_COLL_NO_EXP,
   LANG_COLL_NO_CONTR,
   "b3fb4c073fbc76c5ec302da9128d9542"},
  lang_fastcmp_byte,
  lang_strmatch_iso_88591,
  lang_next_coll_byte,
  lang_split_key_byte,
  lang_init_coll_en_ci
};

static LANG_COLLATION coll_utf8_en_cs = {
  INTL_CODESET_UTF8, 1, 1, DEFAULT_COLL_OPTIONS, NULL,
  /* collation data */
  {LANG_COLL_UTF8_EN_CS, "utf8_en_cs",
   LANG_COLL_GENERIC_SORT_OPT,
   lang_weight_EN_cs, lang_next_alpha_char_EN_cs, LANG_CHAR_COUNT_EN,
   LANG_COLL_NO_EXP,
   LANG_COLL_NO_CONTR,
   "1bdb1b1f630edc508be37f66dfdce7b0"},
  lang_fastcmp_byte,
  lang_strmatch_utf8,
  lang_next_coll_char_utf8,
  lang_split_key_utf8,
  lang_init_coll_utf8_en_cs
};

static LANG_COLLATION coll_utf8_en_ci = {
  INTL_CODESET_UTF8, 1, 1, CI_COLL_OPTIONS, NULL,
  /* collation data */
  {LANG_COLL_UTF8_EN_CI, "utf8_en_ci",
   LANG_COLL_GENERIC_SORT_OPT,
   lang_weight_EN_ci, lang_next_alpha_char_EN_ci, LANG_CHAR_COUNT_EN,
   LANG_COLL_NO_EXP,
   LANG_COLL_NO_CONTR,
   "3050bc8e9814b196f4bbb84759aab77c"},
  lang_fastcmp_byte,
  lang_strmatch_utf8,
  lang_next_coll_char_utf8,
  lang_split_key_utf8,
  lang_init_coll_en_ci
};

static LANG_COLLATION coll_utf8_tr_cs = {
  INTL_CODESET_UTF8, 1, 1, DEFAULT_COLL_OPTIONS, NULL,
  /* collation data */
  {LANG_COLL_UTF8_TR_CS, "utf8_tr_cs",
   LANG_COLL_GENERIC_SORT_OPT,
   lang_weight_TR, lang_next_alpha_char_TR, LANG_CHAR_COUNT_TR,
   LANG_COLL_NO_EXP,
   LANG_COLL_NO_CONTR,
   "52f12f045d2fc90c3a818d0b334485d7"},
  lang_strcmp_utf8,
  lang_strmatch_utf8,
  lang_next_coll_char_utf8,
  lang_split_key_utf8,
  lang_init_coll_utf8_tr_cs
};

static LANG_COLLATION coll_utf8_ko_cs = {
  INTL_CODESET_UTF8, 1, 1, DEFAULT_COLL_OPTIONS, NULL,
  /* collation data - same as en_US.utf8 */
  {LANG_COLL_UTF8_KO_CS, "utf8_ko_cs",
   LANG_COLL_GENERIC_SORT_OPT,
   lang_weight_EN_cs, lang_next_alpha_char_EN_cs, LANG_CHAR_COUNT_EN,
   LANG_COLL_NO_EXP,
   LANG_COLL_NO_CONTR,
   "422c85ede1e265a761078763d2240c81"},
  lang_strcmp_utf8,
  lang_strmatch_utf8,
  lang_next_coll_char_utf8,
  lang_split_key_utf8,
  lang_init_coll_utf8_en_cs
};

static LANG_COLLATION coll_euckr_bin = {
  INTL_CODESET_KSC5601_EUC, 1, 0, DEFAULT_COLL_OPTIONS, NULL,
  /* collation data */
  {LANG_COLL_EUCKR_BINARY, "euckr_bin",
   LANG_COLL_GENERIC_SORT_OPT,
   NULL, NULL, 0,
   LANG_COLL_NO_EXP,
   LANG_COLL_NO_CONTR,
   "18fb633e87f0a3a785ef38cf2a6a7789"},
  lang_fastcmp_ko,
  lang_strmatch_ko,
  lang_next_alpha_char_ko,
  lang_split_key_euc,
  NULL
};


static LANG_COLLATION *built_in_collations[] = {
  &coll_iso_binary,
  &coll_utf8_binary,
  &coll_iso88591_en_cs,
  &coll_iso88591_en_ci,
  &coll_utf8_en_cs,
  &coll_utf8_en_ci,
  &coll_utf8_tr_cs,
  &coll_utf8_ko_cs,
  &coll_euckr_bin,
};

/*
 * lang_init - Initializes any global state required by the multi-language
 *             module
 *   return: true if success
 *
 *  Note : this is a "light" language initialization. User defined locales
 *	   are not loaded during this process and if environment cannot be
 *	   resolved, the default language is set.
 */
bool
lang_init (void)
{
  int i;

  if (lang_Initialized)
    {
      return lang_Initialized;
    }

  /* ignore lang_env_initialized (if already set after catalog message init),
   * and force reading the environment to check any possible errors */
  if (set_lang_from_env () != NO_ERROR)
    {
      return false;
    }

  /* init all collation placeholders with ISO binary collation */
  for (i = 0; i < LANG_MAX_COLLATIONS; i++)
    {
      lang_collations[i] = &coll_iso_binary;
    }

  /* built-in collations : order of registration should match colation ID */
  for (i = 0; i < (int) (sizeof (built_in_collations)
			 / sizeof (built_in_collations[0])); i++)
    {
      (void) register_collation (built_in_collations[i]);
    }

  /* register all built-in locales allowed in current charset
   * Support for multiple locales is required for switching function context
   * string - data/time , string - number conversions */

  /* built-in locales with ISO codeset */
  (void) register_lang_locale_data (&lc_English_iso88591);
  (void) register_lang_locale_data (&lc_Korean_iso88591);
  (void) register_lang_locale_data (&lc_Turkish_iso88591);

  (void) register_lang_locale_data (&lc_Korean_euckr);

  /* built-in locales with UTF-8 codeset : should be loaded last */
  (void) register_lang_locale_data (&lc_English_utf8);
  (void) register_lang_locale_data (&lc_Korean_utf8);
  (void) register_lang_locale_data (&lc_Turkish_utf8);

  /* set current locale */
  set_current_locale (false);

  lang_Initialized = true;

  return (lang_Initialized);
}

/*
 * lang_init_full - Initializes the language according to environment
 *		    including user defined language
 *
 *   return: true if success
 */
bool
lang_init_full (void)
{
  (void) lang_init ();

  if (lang_Fully_Initialized)
    {
      return lang_Fully_Initialized;
    }

  /* re-get variables from environment */
  if (set_lang_from_env () != NO_ERROR)
    {
      return false;
    }

  assert (lang_Initialized == true);

  /* load & register user locales (no matter the default DB codeset) */
  if (init_user_locales () != NO_ERROR)
    {
      set_default_lang ();
      lang_Init_w_error = true;
      lang_Fully_Initialized = true;

      return lang_Fully_Initialized;
    }

  set_current_locale (true);

#if !defined (SERVER_MODE)
  lang_set_client_charset_coll (LANG_SYS_CODESET, LANG_SYS_COLLATION);
#endif

  lang_Fully_Initialized = true;

  return (lang_Fully_Initialized);
}

/*
 * lang_init_console_txt_conv - Initializes console text conversion
 *
 *   return: true if success
 */
void
lang_init_console_txt_conv (void)
{
  char *sys_id = NULL;
  char *conv_sys_ids = NULL;
#if defined(WINDOWS)
  UINT cp;
  char win_codepage_str[32];
#endif

  assert (lang_Loc_data != NULL);

  if (lang_Loc_data == NULL || lang_Loc_data->txt_conv == NULL)
    {
#if !defined(WINDOWS)
      (void) setlocale (LC_CTYPE, "");
#endif
      return;
    }

#if defined(WINDOWS)
  cp = GetConsoleCP ();
  snprintf (win_codepage_str, sizeof (win_codepage_str) - 1, "%d", cp);

  sys_id = win_codepage_str;
  conv_sys_ids = lang_Loc_data->txt_conv->win_codepages;
#else
  /* setlocale with empty string forces the current locale :
   * this is required to retrieve codepage id, but as a side-effect modifies
   * the behavior of string utility functions such as 'snprintf' to support
   * current locale charset */
  if (setlocale (LC_CTYPE, "") != NULL)
    {
      sys_id = nl_langinfo (CODESET);
      conv_sys_ids = lang_Loc_data->txt_conv->nl_lang_str;
    }
#endif

  if (sys_id != NULL && conv_sys_ids != NULL)
    {
      char *conv_sys_end = conv_sys_ids + strlen (conv_sys_ids);
      char *found_token;

      /* supported system identifiers for conversion are separated by
       * comma */
      do
	{
	  found_token = strstr (conv_sys_ids, sys_id);
	  if (found_token == NULL)
	    {
	      break;
	    }

	  if (found_token + strlen (sys_id) >= conv_sys_end
	      || *(found_token + strlen (sys_id)) == ','
	      || *(found_token + strlen (sys_id)) == ' ')
	    {
	      if (lang_Loc_data->txt_conv->init_conv_func != NULL)
		{
		  lang_Loc_data->txt_conv->init_conv_func ();
		}
	      console_conv = lang_Loc_data->txt_conv;
	      break;
	    }
	  else
	    {
	      conv_sys_ids = conv_sys_ids + strlen (sys_id);
	    }
	}
      while (conv_sys_ids < conv_sys_end);
    }
}

/*
 * set_current_locale - sets the current locale according to 'lang_Lang_name'
 *			and 'lang_Loc_charset'
 *
 *  is_full_init(in) : true if this is a full language initialization
 */
static void
set_current_locale (bool is_full_init)
{
  bool found = false;

  lang_get_lang_id_from_name (lang_Lang_name, &lang_Lang_id);

  for (lang_Loc_data = lang_loaded_locales[lang_Lang_id];
       lang_Loc_data != NULL; lang_Loc_data = lang_Loc_data->next_lld)
    {
      assert (lang_Loc_data != NULL);

      if (lang_Loc_data->codeset == lang_Loc_charset
	  && strcasecmp (lang_Lang_name, lang_Loc_data->lang_name) == 0)
	{
	  found = true;
	  break;
	}
    }

  if (!found)
    {
      /* when charset is not UTF-8, full init will not be required */
      if (is_full_init || lang_Loc_charset != INTL_CODESET_UTF8)
	{
	  char err_msg[ERR_MSG_SIZE];

	  lang_Init_w_error = true;
	  snprintf (err_msg, sizeof (err_msg) - 1,
		    "Locale %s.%s was not loaded.\n"
		    " %s not found in cubrid_locales.txt",
		    lang_Lang_name, lang_get_codeset_name (lang_Loc_charset),
		    lang_Lang_name);
	  LOG_LOCALE_ERROR (err_msg, ER_LOC_INIT, false);
	}
      set_default_lang ();
    }

  lang_Loc_currency = lang_Loc_data->default_currency_code;

  /* static globals in db_date.c should also be initialized with the current
   * locale (for parsing local am/pm strings for times) */
  db_date_locale_init ();
}

/*
 * set_lang_from_env - Initializes language variables from environment
 *
 *   return: NO_ERROR if success
 *
 *  Note : This function sets the following global variables according to
 *	   $CUBRID_CHARSET environment variable :
 *	    - lang_user_Loc_name : locale string supplied by user
 *	    - lang_Loc_name : resolved locale string: <lang>.<charset>
 *	    - lang_Lang_name : <lang> string part (without <charset>)
 *	    - lang_Lang_id: id of language
 *	    - lang_Loc_charset : charset id : ISO-8859-1, UTF-8 or EUC-KR
 *	   According to $CUBRID_MSG_LANG:
 *	    - lang_msg_Loc_name : <lang>.<charset>; en_US.utf8;
 *	      if $CUBRID_MSG_LANG is not set, then only $CUBRID_CHARSET is used
 */
static int
set_lang_from_env (void)
{
  const char *env;
  char *charset = NULL;
  char err_msg[ERR_MSG_SIZE];
  bool is_msg_env_set = false;
  int status = NO_ERROR;

  /*
   * Determines the locale by examining environment variables.
   * We check the optional variable CUBRID_MSG_LANG, which decides the
   * locale for catalog messages; if not set CUBRID_CHARSET is used for catalog
   * messages
   * CUBRID_CHARSET is mandatory, it decides the locale in which CUBRID opertes.
   * This controls the system charset and collation, including charset and
   * collation of system tables, charset (and casing rules) of identifiers,
   * default charset and collation of string literals, default locale for
   * string - date /numeric conversion functions.
   */

  env = envvar_get ("MSG_LANG");
  if (env != NULL)
    {
      INTL_CODESET dummy_cs;
      char msg_lang[LANG_MAX_LANGNAME];

      strncpy (lang_msg_Loc_name, env, sizeof (lang_msg_Loc_name));

      status = check_env_lang_val (lang_msg_Loc_name, msg_lang,
				   &charset, &dummy_cs);
      if (status != NO_ERROR)
	{
	  sprintf (err_msg, "invalid value '%s' for CUBRID_MSG_LANG",
		   lang_msg_Loc_name);
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_LOC_INIT, 1, err_msg);
	  strcpy (lang_msg_Loc_name, LANG_NAME_DEFAULT);
	  lang_env_initialized = true;
	  return ER_LOC_INIT;
	}
      else
	{
	  is_msg_env_set = true;
	  if (charset == NULL && strcasecmp (msg_lang, "en_US") != 0)
	    {
	      /* by default all catalog message folders are in .utf8, unless
	       * otherwise specified */
	      assert (strlen (lang_msg_Loc_name) == 5);
	      strcat (lang_msg_Loc_name, ".utf8");
	    }
	}
      charset = NULL;
    }

  env = envvar_get ("CHARSET");
  if (env != NULL)
    {
      strncpy (lang_Loc_name, env, sizeof (lang_Loc_name));
    }
  else
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_LOC_INIT, 1,
	      "CUBRID_CHARSET environment variable is not set");

      if (!is_msg_env_set)
	{
	  strcpy (lang_msg_Loc_name, LANG_NAME_DEFAULT);
	}
      lang_env_initialized = true;
      return ER_LOC_INIT;
    }

  strcpy (lang_user_Loc_name, lang_Loc_name);
  lang_Loc_charset = INTL_CODESET_NONE;
  status = check_env_lang_val (lang_Loc_name, lang_Lang_name, &charset,
			       &lang_Loc_charset);
  if (status != NO_ERROR)
    {
      sprintf (err_msg, "invalid value %s for CUBRID_CHARSET", lang_Loc_name);
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_LOC_INIT, 1, err_msg);
      if (!is_msg_env_set)
	{
	  strcpy (lang_msg_Loc_name, LANG_NAME_DEFAULT);
	}
      lang_env_initialized = true;
      return ER_LOC_INIT;
    }

  if (lang_Loc_charset == INTL_CODESET_NONE)
    {
      /* no charset provided in $CUBRID_CHARSET */
      (void) lang_get_builtin_lang_id_from_name (lang_Lang_name,
						 &lang_Lang_id);
      lang_Loc_charset = lang_get_default_codeset (lang_Lang_id);
      if (!lang_is_codeset_allowed (lang_Lang_id, lang_Loc_charset))
	{
	  set_default_lang ();
	  goto error_codeset;
	}
    }
  else if (lang_Loc_charset != INTL_CODESET_UTF8)
    {
      /* not UTF-8 charset, it has to be a built-in language */
      (void) lang_get_builtin_lang_id_from_name (lang_Loc_name,
						 &lang_Lang_id);
      if (!lang_is_codeset_allowed (lang_Lang_id, lang_Loc_charset))
	{
	  goto error_codeset;
	}
    }

  if (!is_msg_env_set)
    {
      strcpy (lang_msg_Loc_name, lang_Loc_name);
    }

  lang_env_initialized = true;

  return NO_ERROR;

error_codeset:
  sprintf (err_msg, "codeset %s for language %s is not supported", charset,
	   lang_Lang_name);
  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_LOC_INIT, 1, err_msg);

  if (!is_msg_env_set)
    {
      strcpy (lang_msg_Loc_name, LANG_NAME_DEFAULT);
    }
  lang_env_initialized = true;

  return ER_LOC_INIT;
}

/*
 * check_env_lang_val - check and normalizes the environment variable value;
 *			gets the language and charset parts
 *
 *   return: NO_ERROR if success
 *
 *   env_val(in/out): value; Example : "En_US.UTF8" -> en_US.utf8
 *   lang_name(out): language part : en_US
 *   charset_ptr(out): pointer in env_val to charset part : utf8
 *   codeset(out): codeset value, according to charset part or
 *		   INTL_CODESET_NODE, if charset part is empty
 *
 */
static int
check_env_lang_val (char *env_val, char *lang_name, char **charset_ptr,
		    INTL_CODESET * codeset)
{
  char *charset;

  assert (env_val != NULL);
  assert (lang_name != NULL);
  assert (charset_ptr != NULL);

  /* strip quotas : */
  envvar_trim_char (env_val, (int) '\"');

  /* Locale should be formated like xx_XX.charset or xx_XX */
  charset = strchr (env_val, '.');
  *charset_ptr = charset;
  if (charset != NULL)
    {
      strncpy (lang_name, env_val, charset - env_val);
      lang_name[charset - env_val] = '\0';

      charset++;
      if (strcasecmp (charset, LANG_CHARSET_EUCKR) == 0
	  || strcasecmp (charset, LANG_CHARSET_EUCKR_ALIAS1) == 0)
	{
	  *codeset = INTL_CODESET_KSC5601_EUC;
	  strcpy (charset, LANG_CHARSET_EUCKR);
	}
      else if (strcasecmp (charset, LANG_CHARSET_UTF8) == 0
	       || strcasecmp (charset, LANG_CHARSET_UTF8_ALIAS1) == 0)
	{
	  *codeset = INTL_CODESET_UTF8;
	  strcpy (charset, LANG_CHARSET_UTF8);
	}
      else if (strcasecmp (charset, LANG_CHARSET_ISO88591) == 0
	       || strcasecmp (charset, LANG_CHARSET_ISO88591_ALIAS1) == 0
	       || strcasecmp (charset, LANG_CHARSET_ISO88591_ALIAS2) == 0)
	{
	  *codeset = INTL_CODESET_ISO88591;
	  strcpy (charset, LANG_CHARSET_ISO88591);
	}
      else
	{
	  return ER_FAILED;
	}
    }
  else
    {
      strcpy (lang_name, env_val);
    }

  if (strlen (lang_name) == 5)
    {
      intl_toupper_iso8859 (lang_name + 3, 2);
      intl_tolower_iso8859 (lang_name, 2);
    }
  else
    {
      return ER_FAILED;
    }

  memcpy (env_val, lang_name, strlen (lang_name));

  return NO_ERROR;
}

/*
 * set_default_lang -
 *   return:
 *
 */
static void
set_default_lang (void)
{
  lang_Lang_id = INTL_LANG_ENGLISH;
  strncpy (lang_Loc_name, LANG_NAME_DEFAULT, sizeof (lang_Loc_name));
  strncpy (lang_Lang_name, LANG_NAME_DEFAULT, sizeof (lang_Lang_name));
  lang_Loc_data = &lc_English_iso88591;
  lang_Loc_charset = lang_Loc_data->codeset;
  lang_Loc_currency = lang_Loc_data->default_currency_code;
}

/*
 * lang_check_init -
 *   return: error code if language initialization flag is set
 *
 */
bool
lang_check_init (void)
{
  return (!lang_Init_w_error);
}

/*
 * lang_locales_count -
 *   return: number of locales in the system
 */
int
lang_locales_count (bool check_codeset)
{
  int i;
  int count;

  if (!check_codeset)
    {
      return lang_count_locales;
    }

  count = 0;
  for (i = 0; i < lang_count_locales; i++)
    {
      LANG_LOCALE_DATA *lld = lang_loaded_locales[i];
      do
	{
	  count++;
	  lld = lld->next_lld;
	}
      while (lld != NULL);
    }

  return count;
}

/*
 * init_user_locales -
 *   return: error code
 *
 */
static int
init_user_locales (void)
{
  LOCALE_FILE *user_lf = NULL;
  int num_user_loc = 0, i;
  int er_status = NO_ERROR;

  er_status = locale_get_cfg_locales (&user_lf, &num_user_loc, true);
  if (er_status != NO_ERROR)
    {
      goto error;
    }

  loclib_handle_size = num_user_loc;
  loclib_handle_count = 0;

  loclib_handle = (void *) malloc (loclib_handle_size * sizeof (void *));
  if (loclib_handle == NULL)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1,
	      loclib_handle_size * sizeof (void *));
      er_status = ER_OUT_OF_VIRTUAL_MEMORY;
      goto error;
    }

  for (i = 0; i < num_user_loc; i++)
    {
      /* load user locale */
      LANG_LOCALE_DATA *lld = NULL;
      LANG_LOCALE_DATA *last_lang_locale = NULL;
      INTL_LANG l_id;
      bool is_new_locale = false;

      er_status = locale_check_and_set_default_files (&(user_lf[i]), true);
      if (er_status != NO_ERROR)
	{
	  goto error;
	}

      loclib_handle[loclib_handle_count] = NULL;
      er_status = lang_load_library (user_lf[i].lib_file,
				     &(loclib_handle[loclib_handle_count]));
      if (er_status != NO_ERROR)
	{
	  goto error;
	}
      loclib_handle_count++;

      lld = find_lang_locale_data (user_lf[i].locale_name, INTL_CODESET_UTF8,
				   &last_lang_locale);

      if (lld != NULL)
	{
	  /* user customization : overwrite built-in locale */
	  if (lld->is_user_data)
	    {
	      char err_msg[ERR_MSG_SIZE];

	      snprintf (err_msg, sizeof (err_msg) - 1,
			"Duplicate user locale : %s", lld->lang_name);
	      er_status = ER_LOC_INIT;
	      LOG_LOCALE_ERROR (err_msg, er_status, true);
	      goto error;
	    }
	  l_id = lld->lang_id;
	}
      else
	{
	  /* locale not found */
	  if (last_lang_locale != NULL)
	    {
	      /* existing language, but new locale (another charset) */
	      l_id = last_lang_locale->lang_id;
	    }
	  else
	    {
	      /* new language */
	      l_id = lang_count_locales;

	      assert (l_id >= INTL_LANG_USER_DEF_START);

	      if (l_id >= LANG_MAX_LOADED_LOCALES)
		{
		  er_status = ER_LOC_INIT;
		  LOG_LOCALE_ERROR ("too many locales", er_status, true);
		  goto error;
		}
	    }

	  lld = malloc (sizeof (LANG_LOCALE_DATA));
	  if (lld == NULL)
	    {
	      er_status = ER_LOC_INIT;
	      LOG_LOCALE_ERROR ("memory allocation failed", er_status, true);
	      goto error;
	    }

	  memset (lld, 0, sizeof (LANG_LOCALE_DATA));
	  lld->codeset = INTL_CODESET_UTF8;
	  lld->lang_id = l_id;

	  is_new_locale = true;
	}

      assert (lld->codeset == INTL_CODESET_UTF8);
      assert (lld->lang_id == l_id);

      lld->is_user_data = true;

      er_status =
	lang_locale_data_load_from_lib (lld,
					loclib_handle[loclib_handle_count -
						      1], &(user_lf[i]),
					false);
      if (er_status != NO_ERROR)
	{
	  goto error;
	}

      assert (strcmp (lld->lang_name, user_lf[i].locale_name) == 0);

      /* initialization alphabet */
      lld->alphabet.codeset = INTL_CODESET_UTF8;
      lld->ident_alphabet.codeset = INTL_CODESET_UTF8;

      /* initialize text conversion */
      if (lld->txt_conv != NULL)
	{
	  if (lld->txt_conv->conv_type == TEXT_CONV_GENERIC_2BYTE)
	    {
	      lld->txt_conv->init_conv_func = NULL;
	      lld->txt_conv->text_to_utf8_func = intl_text_dbcs_to_utf8;
	      lld->txt_conv->utf8_to_text_func = intl_text_utf8_to_dbcs;
	    }
	  else if (lld->txt_conv->conv_type == TEXT_CONV_GENERIC_1BYTE)
	    {
	      lld->txt_conv->init_conv_func = NULL;
	      lld->txt_conv->text_to_utf8_func =
		intl_text_single_byte_to_utf8;
	      lld->txt_conv->utf8_to_text_func =
		intl_text_utf8_to_single_byte;
	    }
	  else
	    {
	      assert (lld->txt_conv->conv_type == TEXT_CONV_ISO_88591_BUILTIN
		      || lld->txt_conv->conv_type
		      == TEXT_CONV_ISO_88599_BUILTIN);
	    }
	}

      if (lang_get_generic_unicode_norm () == NULL)
	{
	  lang_set_generic_unicode_norm (&(lld->unicode_norm));
	}

      if (is_new_locale)
	{
	  er_status = register_lang_locale_data (lld);

	  if (er_status != NO_ERROR)
	    {
	      goto error;
	    }
	}

      lld->is_initialized = true;
    }

  /* free user defined locale files struct */
  for (i = 0; i < num_user_loc; i++)
    {
      free_and_init (user_lf[i].locale_name);
      free_and_init (user_lf[i].ldml_file);
      free_and_init (user_lf[i].lib_file);
    }

  if (user_lf != NULL)
    {
      free (user_lf);
    }

  return er_status;

error:
  /* free user defined locale files struct */
  for (i = 0; i < num_user_loc; i++)
    {
      free_and_init (user_lf[i].locale_name);
      free_and_init (user_lf[i].ldml_file);
      free_and_init (user_lf[i].lib_file);
    }

  if (user_lf != NULL)
    {
      free (user_lf);
    }

  destroy_user_locales ();
  lang_free_collations ();
  lang_unload_libraries ();

  return er_status;
}

/*
 * register_collation - registers a collation
 *   return: error code
 *   coll(in): collation structure
 */
static int
register_collation (LANG_COLLATION * coll)
{
  int id;
  assert (coll != NULL);
  assert (lang_count_collations < LANG_MAX_COLLATIONS);

  id = coll->coll.coll_id;

  if (id < ((coll->built_in) ? 0 : LANG_MAX_BUILTIN_COLLATIONS)
      || id >= LANG_MAX_COLLATIONS)
    {
      char err_msg[ERR_MSG_SIZE];
      snprintf (err_msg, sizeof (err_msg) - 1,
		"Invalid collation numeric identifier : %d"
		" for collation '%s'. Expecting greater than %d and lower "
		"than %d.", id, coll->coll.coll_name,
		((coll->built_in) ? 0 : LANG_MAX_BUILTIN_COLLATIONS),
		LANG_MAX_COLLATIONS);
      LOG_LOCALE_ERROR (err_msg, ER_LOC_INIT, true);
      return ER_LOC_INIT;
    }

  assert (lang_collations[id] != NULL);

  if (lang_collations[id]->coll.coll_id != LANG_COLL_ISO_BINARY)
    {
      char err_msg[ERR_MSG_SIZE];
      snprintf (err_msg, sizeof (err_msg) - 1,
		"Invalid collation numeric identifier : %d for collation '%s'"
		". This id is already used by collation '%s'",
		id, coll->coll.coll_name,
		lang_collations[id]->coll.coll_name);
      LOG_LOCALE_ERROR (err_msg, ER_LOC_INIT, true);
      return ER_LOC_INIT;
    }

  lang_collations[id] = coll;

  lang_count_collations++;

  if (coll->init_coll != NULL)
    {
      coll->init_coll (coll);
    }

  return NO_ERROR;
}

/*
 * lang_is_coll_name_allowed - checks if collation name is allowed
 *   return: true if allowed
 *   name(in): collation name
 */
bool
lang_is_coll_name_allowed (const char *name)
{
  int i;

  if (name == NULL || *name == '\0')
    {
      return false;
    }

  if (strchr (name, (int) ' ') || strchr (name, (int) '\t'))
    {
      return false;
    }

  for (i = 0; i < (int) (sizeof (built_in_collations)
			 / sizeof (built_in_collations[0])); i++)
    {
      if (strcasecmp (built_in_collations[i]->coll.coll_name, name) == 0)
	{
	  return false;
	}
    }

  return true;
}

/*
 * lang_get_collation - access a collation by id
 *   return: pointer to collation data or NULL
 *   coll_id(in): collation identifier
 */
LANG_COLLATION *
lang_get_collation (const int coll_id)
{
  assert (coll_id >= 0 && coll_id < LANG_MAX_COLLATIONS);

  return lang_collations[coll_id];
}


/*
 * lang_get_collation_name - return collation name
 *   return: collation name
 *   coll_id(in): collation identifier
 */
const char *
lang_get_collation_name (const int coll_id)
{
  if (coll_id < 0 || coll_id >= LANG_MAX_COLLATIONS)
    {
      return NULL;
    }

  return lang_collations[coll_id]->coll.coll_name;
}

/*
 * lang_get_collation_by_name - access a collation by name
 *   return: pointer to collation data or NULL
 *   coll_name(in): collation name
 */
LANG_COLLATION *
lang_get_collation_by_name (const char *coll_name)
{
  int i;
  assert (coll_name != NULL);

  for (i = 0; i < LANG_MAX_COLLATIONS; i++)
    {
      if (strcmp (coll_name, lang_collations[i]->coll.coll_name) == 0)
	{
	  return lang_collations[i];
	}
    }

  return NULL;
}

/*
 * lang_collation_count -
 *   return: number of collations in the system
 */
int
lang_collation_count (void)
{
  return lang_count_collations;
}

/*
 * lang_get_codeset_name - get charset string equivalent
 *   return: charset string or empty string
 *   codeset_id(in): charset/codeset id
 */
const char *
lang_get_codeset_name (int codeset_id)
{
  switch (codeset_id)
    {
    case INTL_CODESET_UTF8:
      return "utf8";
    case INTL_CODESET_ISO88591:
      return "iso88591";
    case INTL_CODESET_KSC5601_EUC:
      return "euckr";
    }

  /* codeset_id is propagated downwards from the grammar, so it is either
   * INTL_CODESET_UTF8, INTL_CODESET_KSC5601_EUC or INTL_CODESET_ISO88591 */
  assert (false);

  return "";
}

/*
 * lang_user_alphabet_w_coll -
 *   return: id of default collation
 */
const ALPHABET_DATA *
lang_user_alphabet_w_coll (const int collation_id)
{
  LANG_COLLATION *lang_coll;

  lang_coll = lang_get_collation (collation_id);

  assert (lang_coll->default_lang != NULL);

  return &(lang_coll->default_lang->alphabet);
}

/*
 * find_lang_locale_data - searches a locale with a given name and codeset
 *   return: locale or NULL if the name+codeset combination was not found
 *   name(in): name of locale
 *   codeset(in): codeset to search
 *   last_locale(out): last locale whith this name or NULL if no locale was
 *		       found
 */
static LANG_LOCALE_DATA *
find_lang_locale_data (const char *name, const INTL_CODESET codeset,
		       LANG_LOCALE_DATA ** last_lang_locale)
{
  LANG_LOCALE_DATA *first_lang_locale = NULL;
  LANG_LOCALE_DATA *curr_lang_locale;
  LANG_LOCALE_DATA *found_lang_locale = NULL;
  int i;

  assert (last_lang_locale != NULL);

  for (i = 0; i < lang_count_locales; i++)
    {
      if (strcasecmp (lang_loaded_locales[i]->lang_name, name) == 0)
	{
	  first_lang_locale = lang_loaded_locales[i];
	  break;
	}
    }

  for (curr_lang_locale = first_lang_locale; curr_lang_locale != NULL;
       curr_lang_locale = curr_lang_locale->next_lld)
    {
      if (codeset == curr_lang_locale->codeset)
	{
	  found_lang_locale = curr_lang_locale;
	}

      if (curr_lang_locale->next_lld == NULL)
	{
	  *last_lang_locale = curr_lang_locale;
	  break;
	}
    }

  return found_lang_locale;
}

/*
 * register_lang_locale_data - registers a language locale data in the system
 *   return: error status
 *   lld(in): language locale data
 */
static int
register_lang_locale_data (LANG_LOCALE_DATA * lld)
{
  LANG_LOCALE_DATA *last_lang_locale = NULL;
  LANG_LOCALE_DATA *found_lang_locale = NULL;

  assert (lld != NULL);

  found_lang_locale = find_lang_locale_data (lld->lang_name, lld->codeset,
					     &last_lang_locale);

  assert (found_lang_locale == NULL);

  if (!lld->is_user_data)
    {
      /* make a copy of built-in */
      LANG_LOCALE_DATA *new_lld =
	(LANG_LOCALE_DATA *) malloc (sizeof (LANG_LOCALE_DATA));
      if (new_lld == NULL)
	{
	  LOG_LOCALE_ERROR ("memory allocation failed", ER_LOC_INIT, true);
	  return ER_LOC_INIT;
	}

      memcpy (new_lld, lld, sizeof (LANG_LOCALE_DATA));
      lld = new_lld;
    }

  if (last_lang_locale == NULL)
    {
      /* no other locales exists with the same name */
      assert (lang_count_locales < LANG_MAX_LOADED_LOCALES);
      lang_loaded_locales[lang_count_locales++] = lld;
    }
  else
    {
      last_lang_locale->next_lld = lld;
    }

  if (!(lld->is_initialized) && lld->initloc != NULL)
    {
      assert (lld->lang_id < (INTL_LANG) INTL_LANG_USER_DEF_START);
      init_builtin_calendar_names (lld);
      lld->initloc (lld);

      /* init default collation */
      if (lld->default_lang_coll != NULL
	  && lld->default_lang_coll->init_coll != NULL)
	{
	  lld->default_lang_coll->init_coll (lld->default_lang_coll);
	}
    }

  return NO_ERROR;
}

/*
 * free_lang_locale_data - Releases any resources held by a language locale
 *			   data
 *   return: none
 */
static void
free_lang_locale_data (LANG_LOCALE_DATA * lld)
{
  assert (lld != NULL);

  if (lld->next_lld != NULL)
    {
      free_lang_locale_data (lld->next_lld);
      lld->next_lld = NULL;
    }

  if (lld->is_user_data)
    {
      /* Text conversions having init_conv_func not NULL are built-in.
       * They can't be deallocated.
       */
      if (lld->txt_conv != NULL && lld->txt_conv->init_conv_func == NULL)
	{
	  free (lld->txt_conv);
	  lld->txt_conv = NULL;
	}
    }

  free (lld);
}

/*
 * lang_get_user_loc_name - returns the string provided by user in CUBRID_CHARSET
 *		            according to environment
 *   return: locale user string
 */
const char *
lang_get_user_loc_name (void)
{
  return lang_user_Loc_name;
}

/*
 * lang_get_msg_Loc_name - returns the language name for the message files,
 *			   according to environment
 *   return: language name string
 */
const char *
lang_get_msg_Loc_name (void)
{
  if (!lang_env_initialized)
    {
      /* ignore any errors, we just need a locale for messages */
      (void) set_lang_from_env ();
    }

  return lang_msg_Loc_name;
}

/*
 * lang_get_Lang_name - returns the language name according to environment
 *   return: language name string
 */
const char *
lang_get_Lang_name (void)
{
  if (!lang_Initialized)
    {
      lang_init ();
    }
  return lang_Lang_name;
}

/*
 * lang_id - Returns language id per env settings
 *   return: language identifier
 */
INTL_LANG
lang_id (void)
{
  if (!lang_Initialized)
    {
      lang_init ();
    }
  return lang_Lang_id;
}

/*
 * lang_currency - Returns language currency per env settings
 *   return: language currency identifier
 */
DB_CURRENCY
lang_currency ()
{
  if (!lang_Initialized)
    {
      lang_init ();
    }
  return lang_Loc_currency;
}

/*
 * lang_charset - Returns language charset per env settings
 *   return: language charset
 */
INTL_CODESET
lang_charset (void)
{
  if (!lang_Initialized)
    {
      lang_init ();
    }
  return lang_Loc_charset;
}

/*
 * lang_final - Releases any resources held by this module
 *   return: none
 */
void
lang_final (void)
{
  destroy_user_locales ();

  lang_free_collations ();

  lang_set_generic_unicode_norm (NULL);

  lang_unload_libraries ();

  lang_Initialized = false;
  lang_Fully_Initialized = false;
}

/*
 * lang_currency_symbol - Computes an appropriate printed representation for
 *                        a currency identifier
 *   return: currency string
 *   curr(in): currency constant
 */
const char *
lang_currency_symbol (DB_CURRENCY curr)
{
  return intl_get_money_symbol_console (curr);
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * lang_char_mem_size - Returns the character memory size for the given
 *                      pointer to a character
 *   return: memory size for the first character
 *   p(in)
 */
int
lang_char_mem_size (const char *p)
{
  if (LANG_VARIABLE_CHARSET (lang_charset ()))
    {
      if (0x80 & (p[0]))
	{
	  return 2;
	}
    }
  return 1;
}

/*
 * lang_char_screen_size - Returns the screen size for the given pointer
 *                         to a character
 *   return: screen size for the first character
 *   p(in)
 */
int
lang_char_screen_size (const char *p)
{
  if (LANG_VARIABLE_CHARSET (lang_charset ()))
    {
      return (0x80 & (p[0]) ? 2 : 1);
    }
  return 1;
}

/*
 * lang_wchar_mem_size - Returns the memory size for the given pointer
 *                       to a wide character
 *   return: memory size for the first character
 *   p(in)
 */
int
lang_wchar_mem_size (const wchar_t * p)
{
  if (LANG_VARIABLE_CHARSET (lang_charset ()))
    {
      if (0x8000 & (p[0]))
	{
	  return 2;
	}
    }
  return 1;
}

/*
 * lang_wchar_screen_size - Returns the screen size for the given pointer
 *                          to a wide character
 *   return: screen size for the first character
 *   p(in)
 */
int
lang_wchar_screen_size (const wchar_t * p)
{
  if (LANG_VARIABLE_CHARSET (lang_charset ()))
    {
      return (0x8000 & (p[0]) ? 2 : 1);
    }
  return 1;
}
#endif

/*
 * lang_check_identifier - Tests an identifier for possibility
 *   return: true if the name is suitable for identifier,
 *           false otherwise.
 *   name(in): identifier name
 *   length(in): identifier name length
 */
bool
lang_check_identifier (const char *name, int length)
{
  bool ok = false;
  int i;

  if (name == NULL)
    {
      return false;
    }

  if (char_isalpha (name[0]))
    {
      ok = true;
      for (i = 0; i < length && ok; i++)
	{
	  if (!char_isalnum (name[i]) && name[i] != '_')
	    {
	      ok = false;
	    }
	}
    }

  return (ok);
}

/*
 * lang_locale - returns language locale per env settings.
 *   return: language locale data
 */
const LANG_LOCALE_DATA *
lang_locale (void)
{
  if (!lang_Initialized)
    {
      lang_init ();
    }
  return lang_Loc_data;
}

/*
 * lang_get_specific_locale - returns language locale of a specific language
 *			      and codeset
 *
 *  return: language locale data
 *  lang(in):
 *  codeset(in):
 *
 *  Note : if codeset is INTL_CODESET_NONE, returns the first locale it
 *	   founds with requested language id, not matter the codeset.
 */
const LANG_LOCALE_DATA *
lang_get_specific_locale (const INTL_LANG lang, const INTL_CODESET codeset)
{
  if (!lang_Initialized)
    {
      lang_init ();
    }

  if ((int) lang < lang_count_locales)
    {
      LANG_LOCALE_DATA *first_lang_locale = lang_loaded_locales[lang];
      LANG_LOCALE_DATA *curr_lang_locale;

      for (curr_lang_locale = first_lang_locale; curr_lang_locale != NULL;
	   curr_lang_locale = curr_lang_locale->next_lld)
	{
	  if (curr_lang_locale->codeset == codeset
	      || codeset == INTL_CODESET_NONE)
	    {
	      return curr_lang_locale;
	    }
	}
    }

  return NULL;
}


/*
 * lang_get_first_locale_for_lang - returns first locale for language
 *  return: language locale data or NULL if language id is not valid
 *  lang(in):
 */
const LANG_LOCALE_DATA *
lang_get_first_locale_for_lang (const INTL_LANG lang)
{
  if (!lang_Initialized)
    {
      lang_init ();
    }

  if ((int) lang < lang_count_locales)
    {
      return lang_loaded_locales[lang];
    }

  return NULL;
}

/*
 * lang_get_builtin_lang_id_from_name - returns the builtin language id from a
 *					language name
 *
 *   return: 0, if language name is accepted, non-zero otherwise
 *   lang_name(in):
 *   lang_id(out): language identifier
 *
 *  Note : INTL_LANG_ENGLISH is returned if name is not a valid language name
 */
static int
lang_get_builtin_lang_id_from_name (const char *lang_name,
				    INTL_LANG * lang_id)
{
  int i;

  assert (lang_id != NULL);

  *lang_id = INTL_LANG_ENGLISH;

  for (i = 0; i < (int) (sizeof (builtin_langs) / sizeof (LANG_DEFAULTS));
       i++)
    {
      if (strncasecmp (lang_name, builtin_langs[i].lang_name,
		       strlen (builtin_langs[i].lang_name)) == 0)
	{
	  *lang_id = builtin_langs[i].lang;
	  return 0;
	}
    }

  assert (*lang_id < INTL_LANG_USER_DEF_START);

  return 1;
}

/*
 * lang_get_lang_id_from_name - returns the language id from a language name
 *
 *   return: 0, if language name is accepted, non-zero otherwise
 *   lang_name(in):
 *   lang_id(out): language identifier
 *
 *  Note : INTL_LANG_ENGLISH is returned if name is not a valid language name
 */
int
lang_get_lang_id_from_name (const char *lang_name, INTL_LANG * lang_id)
{
  int i;

  assert (lang_id != NULL);

  *lang_id = INTL_LANG_ENGLISH;

  for (i = 0; i < lang_count_locales; i++)
    {
      assert (lang_loaded_locales[i] != NULL);

      if (strcasecmp (lang_name, lang_loaded_locales[i]->lang_name) == 0)
	{
	  assert (i == (int) lang_loaded_locales[i]->lang_id);
	  *lang_id = lang_loaded_locales[i]->lang_id;
	  return 0;
	}
    }

  return 1;
}

/*
 * lang_get_lang_name_from_id - returns the language name from a language id
 *
 *   return: language name (NULL if lang_id is not valid)
 *   lang_id(in):
 *
 */
const char *
lang_get_lang_name_from_id (const INTL_LANG lang_id)
{
  if ((int) lang_id < lang_count_locales)
    {
      assert (lang_loaded_locales[lang_id] != NULL);
      return lang_loaded_locales[lang_id]->lang_name;
    }

  return NULL;
}

/*
 * lang_set_flag_from_lang - set a flag according to language string
 *
 *   return: 0 if language string OK and flag was set, non-zero otherwise
 *   lang_str(in): language string identier
 *   has_user_format(in): true if user has given a format, false otherwise
 *   has_user_lang(in): true if user has given a language, false otherwise
 *   flag(out): bit flag : bit 0 is the user flag, bits 1 - 31 are for
 *		language identification
 *		Bit 0 : if set, the language was given by user
 *		Bit 1 - 31 : INTL_LANG
 *
 *  Note : function is used in context of some date-string functions.
 *	   If lang_str cannot be solved, the language is assumed English.
 */
int
lang_set_flag_from_lang (const char *lang_str, bool has_user_format,
			 bool has_user_lang, int *flag)
{
  INTL_LANG lang = INTL_LANG_ENGLISH;
  int status = 0;

  if (lang_str != NULL)
    {
      status = lang_get_lang_id_from_name (lang_str, &lang);
    }

  if (lang_set_flag_from_lang_id (lang, has_user_format, has_user_lang,
				  flag) == 0)
    {
      return status;
    }

  assert (lang == INTL_LANG_ENGLISH);

  return 1;
}

/*
 * lang_set_flag_from_lang - set a flag according to language identifier
 *
 *   return: 0 if language string OK and flag was set, non-zero otherwise
 *   lang(in): language identier
 *   has_user_format(in): true if user has given a format, false otherwise
 *   has_user_lang(in): true if user has given a language, false otherwise
 *   flag(out): bit flag : bits 0 and 1 are user flags, bits 2 - 31 are for
 *		language identification
 *		Bit 0 : if set, the format was given by user
*		Bit 1 : if set, the language was given by user
 *		Bit 2 - 31 : INTL_LANG
 *		Consider change this flag to store the language as value
 *		instead of as bit map
 *
 *  Note : function is used in context of some date-string functions.
 */
int
lang_set_flag_from_lang_id (const INTL_LANG lang, bool has_user_format,
			    bool has_user_lang, int *flag)
{
  int lang_val = (int) lang;

  *flag = 0;

  *flag |= (has_user_format) ? 1 : 0;
  *flag |= (has_user_lang) ? 2 : 0;

  if (lang_val >= lang_count_locales)
    {
      lang_val = (int) INTL_LANG_ENGLISH;
      *flag |= lang_val << 2;
      return 1;
    }

  *flag |= lang_val << 2;

  return 0;
}

/*
 * lang_get_lang_id_from_flag - get lang id from flag
 *
 *   return: id of language, current language is returned when flag value is
 *	     invalid
 *   flag(in): bit flag : bit 0 and 1 are user flags, bits 2 - 31 are for
 *	       language identification
 *
 *  Note : function is used in context of some date-string functions.
 */
INTL_LANG
lang_get_lang_id_from_flag (const int flag, bool * has_user_format,
			    bool * has_user_lang)
{
  int lang_val;

  *has_user_format = ((flag & 0x1) == 0x1) ? true : false;
  *has_user_lang = ((flag & 0x2) == 0x2) ? true : false;

  lang_val = flag >> 2;

  if (lang_val >= 0 && lang_val < lang_count_locales)
    {
      return (INTL_LANG) lang_val;
    }

  return lang_id ();
}

/*
 * lang_date_format - Returns the default format of date for the required
 *		      language or NULL if a the default format is not
 *		      available
 *   lang_id (in):
 *   codeset (in):
 *   type (in): DB type for format
 */
const char *
lang_date_format (const INTL_LANG lang_id, const INTL_CODESET codeset,
		  const DB_TYPE type)
{
  const LANG_LOCALE_DATA *lld;

  assert (lang_Fully_Initialized);

  lld = lang_get_specific_locale (lang_id, codeset);

  if (lld == NULL)
    {
      return NULL;
    }

  switch (type)
    {
    case DB_TYPE_TIME:
      return lld->time_format;
    case DB_TYPE_DATE:
      return lld->date_format;
    case DB_TYPE_DATETIME:
      return lld->datetime_format;
    case DB_TYPE_TIMESTAMP:
      return lld->timestamp_format;
    default:
      break;
    }

  return NULL;
}

/*
 * lang_get_default_codeset - returns the default codeset to be used for a
 *			      given language identifier
 *   return: codeset
 *   intl_id(in):
 */
static INTL_CODESET
lang_get_default_codeset (const INTL_LANG intl_id)
{
  unsigned int i;
  INTL_CODESET codeset = INTL_CODESET_NONE;

  for (i = 0; i < sizeof (builtin_langs) / sizeof (LANG_DEFAULTS); i++)
    {
      if (intl_id == builtin_langs[i].lang)
	{
	  codeset = builtin_langs[i].codeset;
	  break;
	}
    }
  return codeset;
}

/*
 * lang_is_codeset_allowed - checks if a combination of language and codeset
 *			     is allowed
 *   return: true if combination is allowed, false otherwise
 *   intl_id(in):
 *   codeset(in):
 */
static bool
lang_is_codeset_allowed (const INTL_LANG intl_id, const INTL_CODESET codeset)
{
  unsigned int i;

  for (i = 0; i < sizeof (builtin_langs) / sizeof (LANG_DEFAULTS); i++)
    {
      if (intl_id == builtin_langs[i].lang &&
	  codeset == builtin_langs[i].codeset)
	{
	  return true;
	}
    }
  return false;
}

/*
 * lang_digit_grouping_symbol - Returns symbol used for grouping digits in
 *				numbers
 *   lang_id (in):
 */
char
lang_digit_grouping_symbol (const INTL_LANG lang_id)
{
  const LANG_LOCALE_DATA *lld =
    lang_get_specific_locale (lang_id, INTL_CODESET_NONE);

  assert (lld != NULL);

  return lld->number_group_sym;
}

/*
 * lang_digit_fractional_symbol - Returns symbol used for fractional part of
 *				  numbers
 *   lang_id (in):
 */
char
lang_digit_fractional_symbol (const INTL_LANG lang_id)
{
  const LANG_LOCALE_DATA *lld =
    lang_get_specific_locale (lang_id, INTL_CODESET_NONE);

  assert (lld != NULL);

  return lld->number_decimal_sym;
}

/*
 * lang_get_txt_conv - Returns the information required for console text
 *		       conversion
 */
TEXT_CONVERSION *
lang_get_txt_conv (void)
{
  return console_conv;
}

/*
 * lang_charset_name() - returns charset name
 *
 *   return:
 *   codeset(in):
 */
const char *
lang_charset_name (const INTL_CODESET codeset)
{
  int i;

  assert (codeset >= INTL_CODESET_ISO88591 && codeset <= INTL_CODESET_UTF8);

  for (i = 0; lang_Db_charsets[i].charset_id != INTL_CODESET_NONE; i++)
    {
      if (codeset == lang_Db_charsets[i].charset_id)
	{
	  return lang_Db_charsets[i].charset_name;
	}
    }

  return NULL;
}

/*
 * lang_charset_cubrid_name() - returns charset name
 *
 *   return:
 *   codeset(in):
 */
const char *
lang_charset_cubrid_name (const INTL_CODESET codeset)
{
  int i;

  assert (codeset >= INTL_CODESET_ISO88591 && codeset <= INTL_CODESET_UTF8);

  for (i = 0; lang_Db_charsets[i].charset_id != INTL_CODESET_NONE; i++)
    {
      if (codeset == lang_Db_charsets[i].charset_id)
	{
	  return lang_Db_charsets[i].charset_cubrid_name;
	}
    }

  return NULL;
}

/*
 * lang_get_charset_env_string -
 * buf(out):
 * buf_size(in):
 * lang_name(in):
 * codeset(in):
 * return:
 */
int
lang_get_charset_env_string (char *buf, int buf_size, const char *lang_name,
			     const INTL_CODESET codeset)
{
  if (buf == NULL)
    {
      assert_release (0);
      return ER_FAILED;
    }

  if (!strcasecmp (lang_name, "en_US") && codeset == INTL_CODESET_ISO88591)
    {
      snprintf (buf, buf_size, "%s", lang_name);
    }
  else
    {
      snprintf (buf, buf_size, "%s.%s", lang_name,
		lang_charset_cubrid_name (codeset));
    }

  return NO_ERROR;
}

#if !defined (SERVER_MODE)
static DB_CHARSET lang_Server_charset;
static INTL_LANG lang_Server_lang_id;
static char lang_Server_lang_name[LANG_MAX_LANGNAME + 1];

static int lang_Server_charset_Initialized = 0;

/* client side charset and collation */
static INTL_CODESET lang_Client_charset = INTL_CODESET_ISO88591;
static int lang_Client_collation_id = LANG_COLL_ISO_BINARY;
static int lang_Client_charset_Initialized = 0;
static bool lang_Parser_use_client_charset = true;

/*
 * lang_get_server_charset_env_string -
 * buf(out):
 * buf_size(in):
 * return:
 */
int
lang_get_server_charset_env_string (char *buf, int buf_size)
{
  int ret;

  if (buf == NULL)
    {
      assert_release (0);
      return ER_FAILED;
    }

  if (!lang_Server_charset_Initialized)
    {
      lang_server_charset_init ();
    }

  ret = lang_get_charset_env_string (buf, buf_size, lang_Server_lang_name,
				     lang_Server_charset.charset_id);

  return ret;
}

/*
 * lang_get_client_charset_env_string -
 * buf(out):
 * buf_size(in):
 * return:
 */
int
lang_get_client_charset_env_string (char *buf, int buf_size)
{
  int ret;

  if (buf == NULL)
    {
      assert_release (0);
      return ER_FAILED;
    }

  if (!lang_Initialized)
    {
      lang_init ();
    }

  ret =
    lang_get_charset_env_string (buf, buf_size,
				 lang_get_lang_name_from_id (lang_id ()),
				 lang_charset ());

  return ret;
}

/*
 * lang_server_charset_init - Initializes the global value of the server's
 *                            charset
 *   return: none
 *
 * Note: This is the charset that is bound to the database at the time
 *       of creation.
 */
void
lang_server_charset_init (void)
{
  DB_VALUE value;
  INTL_CODESET srvr_codeset;
  int i;

  /* Determine the Server's charset */

  /* Currently can't read the db_root table while on the server.
   * Temporarily just get the server's codeset from the locale if
   * on the server.
   *
   * The following is safe since this is a client only function. If this
   * needs to move to the server, the db_get must be pre-processed out
   * or something ...
   */
  if (Au_root)
    {
      /* Can't find the server's codeset.  This should only happen if using
       * a database prior to NCHAR implementation, or prior to completely
       * logging in.  If so, set the server codeset to be ASCII until
       * the the db_get can work correctly.  All string handling prior
       * to that time will be done without conversion.
       */
      if (db_get (Au_root, "charset", &value) != NO_ERROR)
	{
	  srvr_codeset = lang_charset ();
	}
      else
	{
	  /* Set the initialized flag */
	  lang_Server_charset_Initialized = 1;
	  assert (DB_VALUE_TYPE (&value) == DB_TYPE_INTEGER);
	  srvr_codeset = (INTL_CODESET) db_get_int (&value);
	}

      if (db_get (Au_root, "lang", &value) != NO_ERROR)
	{
	  lang_Server_lang_id = lang_id ();
	  strcpy (lang_Server_lang_name,
		  lang_get_lang_name_from_id (lang_Server_lang_id));
	}
      else
	{
	  char *db_lang;

	  assert (DB_VALUE_TYPE (&value) == DB_TYPE_STRING);
	  db_lang = db_get_string (&value);

	  if (db_lang != NULL)
	    {
	      int lang_len = MIN (strlen (db_lang), LANG_MAX_LANGNAME);

	      strncpy (lang_Server_lang_name, db_lang, lang_len);
	      lang_Server_lang_name[lang_len] = '\0';

	      lang_get_lang_id_from_name (lang_Server_lang_name,
					  &lang_Server_lang_id);
	    }
	  else
	    {
	      lang_Server_lang_id = lang_id ();
	      strcpy (lang_Server_lang_name,
		      lang_get_lang_name_from_id (lang_Server_lang_id));
	    }
	}
    }
  else
    {
      srvr_codeset = lang_charset ();
    }

  /* Find the charset in the Db_Charsets array */
  for (i = 0; lang_Db_charsets[i].charset_id != INTL_CODESET_NONE; i++)
    {
      if (lang_Db_charsets[i].charset_id == srvr_codeset)
	{
	  lang_Server_charset = lang_Db_charsets[i];
	  return;
	}
    }

  /* Server's codeset not found in the list; Initialize to ASCII */
  lang_Server_charset = lang_Db_charsets[0];
  return;
}

/*
 * lang_set_national_charset - Changes the charset definition for NCHAR
 *   return: error code
 *   charset_name(in): desired charset name
 *
 * Note: This should be called only by the DBA.
 */
int
lang_set_national_charset (const char *charset_name)
{
  INTL_CODESET server_codeset;
  INTL_LANG server_lang;
  DB_VALUE value;
  int au_save;

  if (charset_name == NULL)
    {
      server_codeset = lang_charset ();
    }
  else
    {
      if (lang_charset_name_to_id (charset_name, &server_codeset) != NO_ERROR)
	{
	  server_codeset = lang_charset ();
	}
    }

  server_lang = lang_id ();

  AU_DISABLE (au_save);
  db_make_string (&value, lang_get_lang_name_from_id (server_lang));
  if (db_put_internal (Au_root, "lang", &value) != NO_ERROR)
    {
      /* Error Setting the language */
      assert (false);
    }

  db_make_int (&value, (int) server_codeset);
  if (db_put_internal (Au_root, "charset", &value) != NO_ERROR)
    {
      /* Error Setting the nchar codeset */
      assert (false);
    }
  AU_ENABLE (au_save);

  return NO_ERROR;
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * lang_server_db_charset - Initializes if necessary, then return server's
 *                          charset
 *   return: DB_CHARSET structure associated with the server
 */
DB_CHARSET
lang_server_db_charset (void)
{
  if (!lang_Server_charset_Initialized)
    {
      lang_server_charset_init ();
    }
  return lang_Server_charset;
}
#endif

/*
 * lang_server_charset_id - Initializes if necessary, then return server's
 *                          charset_id
 *   return: INTL_CODESET of the server's charset
 */
INTL_CODESET
lang_server_charset_id (void)
{
  if (!lang_Server_charset_Initialized)
    {
      lang_server_charset_init ();
    }
  return lang_Server_charset.charset_id;
}

/*
 * lang_check_server_env - checks if server settings match with existing
 *			   environment
 *
 *   return: true if server settings match
 */
bool
lang_check_server_env ()
{
  if (!lang_Server_charset_Initialized)
    {
      lang_server_charset_init ();
    }

  if (lang_Server_charset.charset_id != lang_charset ()
      || lang_Server_lang_id != lang_id ())
    {
      return false;
    }

  return true;
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * lang_server_space_char - Initializes if necessary, then return server's
 *                          space character
 *   return: none
 *   space(out): string containing the space character for the server's charset
 *   size(out): number of bytes in the space char
 */
void
lang_server_space_char (char *space, int *size)
{
  if (!lang_Server_charset_Initialized)
    {
      lang_server_charset_init ();
    }
  (void) strcpy (space, lang_Server_charset.space_char);
  *size = lang_Server_charset.space_size;
}

/*
 * lang_server_charset_name - Initializes if necessary, then return server's
 *                            charset name
 *   return: none
 *   name(out): the name of the server's charset
 */
void
lang_server_charset_name (char *name)
{
  if (!lang_Server_charset_Initialized)
    {
      lang_server_charset_init ();
    }
  (void) strcpy (name, lang_Server_charset.charset_name);
}

/*
 * lang_server_charset_desc - Initializes if necessary, then return server's
 *                            charset desc
 *   return: none
 *   desc(out): the description of the server's charset
 */
void
lang_server_charset_desc (char *desc)
{
  if (!lang_Server_charset_Initialized)
    {
      lang_server_charset_init ();
    }
  (void) strcpy (desc, lang_Server_charset.charset_desc);
}
#endif

/*
 * lang_charset_name_to_id - Returns the INTL_CODESET of the specified charset
 *   return: NO_ERROR or error code if the specified name can't be found in
 *           the lang_Db_charsets array
 *   name(in): the name of the desired charset
 *   codeset(out): INTL_CODESET of the desired charset
 */
int
lang_charset_name_to_id (const char *name, INTL_CODESET * codeset)
{
  int i;

  /* Find the charset in the lang_Db_charsets array */
  for (i = 0; lang_Db_charsets[i].charset_id != INTL_CODESET_NONE; i++)
    {
      if (strcmp (lang_Db_charsets[i].charset_name, name) == 0)
	{
	  *codeset = lang_Db_charsets[i].charset_id;
	  return NO_ERROR;
	}
    }

  return ER_FAILED;
}

/*
 * lang_charset_space_char - Returns the space character and its size in bytes
 *                           for a codeset
 *   return: NO_ERROR or error code
 *   codeset(in): INTL_CODESET of the desired charset
 *   space_char(out): character string holding the space character
 *   space_size(out): the number of bytes in the space character
 *
 * Note: This routine assumes that the calling routine has allocated
 *       enough space for space_char, which will use 3 bytes at a maximum.
 */
int
lang_charset_space_char (INTL_CODESET codeset, char *space_char,
			 int *space_size)
{
  int i;

  /* Find the charset in the Db_Charsets array */
  for (i = 0; lang_Db_charsets[i].charset_id != INTL_CODESET_NONE; i++)
    {
      if (lang_Db_charsets[i].charset_id == codeset)
	{
	  *space_size = lang_Db_charsets[i].space_size;
	  (void) memcpy (space_char, lang_Db_charsets[i].space_char,
			 (*space_size));
	  return NO_ERROR;
	}
    }

  *space_size = 0;
  space_char[0] = '\0';
  return ER_FAILED;
}

/*
 * lang_set_client_charset_coll - Sets Client's charset and collation
 *   return: none
 */
void
lang_set_client_charset_coll (const INTL_CODESET codeset,
			      const int collation_id)
{
  assert (codeset == INTL_CODESET_ISO88591
	  || codeset == INTL_CODESET_UTF8
	  || codeset == INTL_CODESET_KSC5601_EUC);

  assert (collation_id >= 0 && collation_id < LANG_MAX_COLLATIONS);

  lang_Client_charset = codeset;
  lang_Client_collation_id = collation_id;
  lang_Client_charset_Initialized = 1;
}

/*
 * lang_get_client_charset - Gets Client's charset
 *   return: codeset
 */
INTL_CODESET
lang_get_client_charset (void)
{
  if (!lang_Client_charset_Initialized)
    {
      lang_set_client_charset_coll (LANG_SYS_CODESET, LANG_SYS_COLLATION);
    }

  return lang_Client_charset;
}

/*
 * lang_get_client_collation - Gets Client's charset
 *   return: codeset
 */
int
lang_get_client_collation (void)
{
  if (!lang_Client_charset_Initialized)
    {
      lang_set_client_charset_coll (LANG_SYS_CODESET, LANG_SYS_COLLATION);
    }

  return lang_Client_collation_id;
}

/*
 * lang_set_parser_use_client_charset - set if next parsing operation should
 *				        use client's setting of charset and
 *					collation
 */
void
lang_set_parser_use_client_charset (bool use)
{
  lang_Parser_use_client_charset = use;
}

/*
 * lang_get_parser_use_client_charset - checks if parser should use client's
 *					charset and collation
 *   return:
 */
bool
lang_get_parser_use_client_charset (void)
{
  return lang_Parser_use_client_charset;
}

#endif /* !SERVER_MODE */

/*
 * lang_charset_introducer() - returns introducer text to print for a charset
 *
 *   return: charset introducer or NULL if not found
 *   codeset(in):
 */
const char *
lang_charset_introducer (const INTL_CODESET codeset)
{
  int i;

  assert (codeset >= INTL_CODESET_ISO88591 && codeset <= INTL_CODESET_UTF8);

  for (i = 0; lang_Db_charsets[i].charset_id != INTL_CODESET_NONE; i++)
    {
      if (codeset == lang_Db_charsets[i].charset_id)
	{
	  return lang_Db_charsets[i].introducer;
	}
    }

  return NULL;
}


/* Collation functions */

/*
 * lang_strcmp_utf8() - string compare for UTF8
 *   return:
 *   lang_coll(in) : collation data
 *   string1(in):
 *   size1(in):
 *   string2(in):
 *   size2(in):
 */
static int
lang_strcmp_utf8 (const LANG_COLLATION * lang_coll,
		  const unsigned char *str1, const int size1,
		  const unsigned char *str2, const int size2)
{
  return lang_strmatch_utf8 (lang_coll, false, str1, size1, str2, size2,
			     NULL, false, NULL);
}

/*
 * lang_strmatch_utf8() - string match and compare for UTF8 collations
 *
 *   return: negative if str1 < str2, positive if str1 > str2, zero otherwise
 *   lang_coll(in) : collation data
 *   is_match(in) : true if match, otherwise is compare
 *   str1(in):
 *   size1(in):
 *   str2(in): this is the pattern string in case of match
 *   size2(in):
 *   escape(in): pointer to escape character (multi-byte allowed)
 *		 (used in context of LIKE)
 *   has_last_escape(in): true if it should check if last character is the
 *			  escape character
 *   str1_match_size(out): size from str1 which is matched with str2
 */
static int
lang_strmatch_utf8 (const LANG_COLLATION * lang_coll, bool is_match,
		    const unsigned char *str1, int size1,
		    const unsigned char *str2, int size2,
		    const unsigned char *escape, const bool has_last_escape,
		    int *str1_match_size)
{
  const unsigned char *str1_end;
  const unsigned char *str2_end;
  const unsigned char *str1_begin;
  const unsigned char *str2_begin;
  unsigned char *str1_next, *str2_next;
  unsigned int cp1, cp2, w_cp1, w_cp2;
  const int alpha_cnt = lang_coll->coll.w_count;
  const unsigned int *weight_ptr = lang_coll->coll.weights;

  str1_begin = str1;
  str2_begin = str2;
  str1_end = str1 + size1;
  str2_end = str2 + size2;

  for (; str1 < str1_end && str2 < str2_end;)
    {
      assert (str1_end - str1 > 0);
      assert (str2_end - str2 > 0);

      cp1 = intl_utf8_to_cp (str1, str1_end - str1, &str1_next);
      cp2 = intl_utf8_to_cp (str2, str2_end - str2, &str2_next);

      if (is_match && escape != NULL
	  && memcmp (str2, escape, str2_next - str2) == 0)
	{
	  if (!(has_last_escape && str2_next >= str2_end))
	    {
	      str2 = str2_next;
	      cp2 = intl_utf8_to_cp (str2, str2_end - str2, &str2_next);
	    }
	}

      if (cp1 < (unsigned int) alpha_cnt)
	{
	  w_cp1 = weight_ptr[cp1];
	}
      else
	{
	  w_cp1 = cp1;
	}

      if (cp2 < (unsigned int) alpha_cnt)
	{
	  w_cp2 = weight_ptr[cp2];
	}
      else
	{
	  w_cp2 = cp2;
	}

      if (w_cp1 != w_cp2)
	{
	  return (w_cp1 < w_cp2) ? (-1) : 1;
	}

      str1 = str1_next;
      str2 = str2_next;
    }

  size1 = str1_end - str1;
  size2 = str2_end - str2;

  assert (size1 == 0 || size2 == 0);

  if (is_match)
    {
      assert (str1_match_size != NULL);
      *str1_match_size = str1 - str1_begin;
    }

  if (size1 == size2)
    {
      return 0;
    }
  else if (size2 > 0)
    {
      if (is_match)
	{
	  return -1;
	}

      for (; str2 < str2_end;)
	{
	  /* ignore trailing white spaces */
	  if (*str2 != 32 && *str2 != 0)
	    {
	      return -1;
	    }
	  str2 += intl_Len_utf8_char[*str2];
	}
    }
  else
    {
      assert (size1 > 0);

      if (is_match)
	{
	  return 0;
	}

      for (; str1 < str1_end;)
	{
	  /* ignore trailing white spaces */
	  if (*str1 != 32 && *str1 != 0)
	    {
	      return 1;
	    }
	  str1 += intl_Len_utf8_char[*str1];
	}
    }

  return 0;
}

/*
 * lang_strcmp_utf8_w_contr() - string compare for UTF8 for a collation
 *				having UCA contractions
 *   return:
 *   lang_coll(in) : collation data
 *   string1(in):
 *   size1(in):
 *   string2(in):
 *   size2(in):
 */
static int
lang_strcmp_utf8_w_contr (const LANG_COLLATION * lang_coll,
			  const unsigned char *str1, const int size1,
			  const unsigned char *str2, const int size2)
{
  return lang_strmatch_utf8_w_contr (lang_coll, false, str1, size1,
				     str2, size2, NULL, false, NULL);
}

/*
 * lang_strmatch_utf8_w_contr() - string match or compare for UTF8 for a
 *				  collation having UCA contractions
 *   return: negative if str1 < str2, positive if str1 > str2, zero otherwise
 *   lang_coll(in) : collation data
 *   is_match(in) : true if match, otherwise is compare
 *   str1(in):
 *   size1(in):
 *   str2(in): this is the pattern string in case of match
 *   size2(in):
 *   escape(in): pointer to escape character (multi-byte allowed)
 *		 (used in context of LIKE)
 *   has_last_escape(in): true if it should check if last character is the
 *			  escape character
 *   str1_match_size(out): size from str1 which is matched with str2
 */
static int
lang_strmatch_utf8_w_contr (const LANG_COLLATION * lang_coll, bool is_match,
			    const unsigned char *str1, int size1,
			    const unsigned char *str2, int size2,
			    const unsigned char *escape,
			    const bool has_last_escape, int *str1_match_size)
{
  const unsigned char *str1_end;
  const unsigned char *str2_end;
  const unsigned char *str1_begin;
  const unsigned char *str2_begin;
  unsigned char *str1_next, *str2_next;
  unsigned int cp1, cp2, w_cp1, w_cp2;
  const COLL_DATA *coll = &(lang_coll->coll);
  const int alpha_cnt = coll->w_count;
  const unsigned int *weight_ptr = lang_coll->coll.weights;
  bool is_str1_contr = false;
  bool is_str2_contr = false;

  str1_end = str1 + size1;
  str2_end = str2 + size2;
  str1_begin = str1;
  str2_begin = str2;

  for (; str1 < str1_end && str2 < str2_end;)
    {
      assert (str1_end - str1 > 0);
      assert (str2_end - str2 > 0);

      cp1 = intl_utf8_to_cp (str1, str1_end - str1, &str1_next);
      cp2 = intl_utf8_to_cp (str2, str2_end - str2, &str2_next);

      if (is_match && escape != NULL
	  && memcmp (str2, escape, str2_next - str2) == 0)
	{
	  if (!(has_last_escape && str2_next >= str2_end))
	    {
	      str2 = str2_next;
	      cp2 = intl_utf8_to_cp (str2, str2_end - str2, &str2_next);
	    }
	}

      is_str1_contr = is_str2_contr = false;

      if (cp1 < (unsigned int) alpha_cnt)
	{
	  COLL_CONTRACTION *contr = NULL;

	  if (str1_end - str1 >= coll->contr_min_size &&
	      cp1 >= coll->cp_first_contr_offset &&
	      cp1 < (coll->cp_first_contr_offset +
		     coll->cp_first_contr_count) &&
	      ((contr =
		lang_get_contr_for_string (coll, str1,
					   str1_end - str1, cp1)) != NULL))
	    {
	      assert (contr != NULL);

	      w_cp1 = contr->wv;
	      str1_next = (unsigned char *) str1 + contr->size;
	      is_str1_contr = true;
	    }
	  else
	    {
	      w_cp1 = weight_ptr[cp1];
	    }
	}
      else
	{
	  w_cp1 = cp1;
	}

      if (cp2 < (unsigned int) alpha_cnt)
	{
	  COLL_CONTRACTION *contr = NULL;

	  if (str2_end - str2 >= coll->contr_min_size &&
	      cp2 >= coll->cp_first_contr_offset &&
	      cp2 < (coll->cp_first_contr_offset +
		     coll->cp_first_contr_count) &&
	      ((contr =
		lang_get_contr_for_string (coll, str2,
					   str2_end - str2, cp2)) != NULL))
	    {
	      assert (contr != NULL);

	      w_cp2 = contr->wv;
	      str2_next = (unsigned char *) str2 + contr->size;
	      is_str2_contr = true;
	    }
	  else
	    {
	      w_cp2 = weight_ptr[cp2];
	    }
	}
      else
	{
	  w_cp2 = cp2;
	}

      if (is_match
	  && coll->uca_opt.sett_match_contr == MATCH_CONTR_BOUND_ALLOW
	  && !is_str2_contr && is_str1_contr && cp1 == cp2)
	{
	  /* re-read weight for str1 ignoring contractions */
	  if (cp1 < (unsigned int) alpha_cnt)
	    {
	      w_cp1 = weight_ptr[cp1];
	    }
	  else
	    {
	      w_cp1 = cp1;
	    }
	  str1_next = (unsigned char *) str1 + intl_Len_utf8_char[*str1];
	}

      if (w_cp1 != w_cp2)
	{
	  return (w_cp1 < w_cp2) ? (-1) : 1;
	}

      str1 = str1_next;
      str2 = str2_next;
    }

  size1 = str1_end - str1;
  size2 = str2_end - str2;

  assert (size1 == 0 || size2 == 0);

  if (is_match)
    {
      assert (str1_match_size != NULL);
      *str1_match_size = size1;
    }

  if (size1 == size2)
    {
      return 0;
    }
  else if (size2 > 0)
    {
      if (is_match)
	{
	  return -1;
	}

      for (; str2 < str2_end;)
	{
	  /* ignore trailing white spaces */
	  if (*str2 != 32 && *str2 != 0)
	    {
	      return -1;
	    }
	  str2 += intl_Len_utf8_char[*str2];
	}
    }
  else
    {
      assert (size1 > 0);
      if (is_match)
	{
	  return 0;
	}
      for (; str1 < str1_end;)
	{
	  /* ignore trailing white spaces */
	  if (*str1 != 32 && *str1 != 0)
	    {
	      return 1;
	    }
	  str1 += intl_Len_utf8_char[*str1];
	}
    }

  return 0;
}

/*
 * lang_get_w_first_el() - get the weight of the first element (character or
 *			   contraction) encountered in the string
 *
 *   return: weight value
 *   coll_data(in): collation data
 *   str(in): buffer to check for contractions
 *   str_size(in): size of buffer (bytes)
 *   next_char(out): pointer to the end of element (next character)
 *
 *   Note : This function works only on UTF-8 collations without expansions.
 *
 */
static unsigned int
lang_get_w_first_el (const COLL_DATA * coll,
		     const unsigned char *str, const int str_size,
		     unsigned char **next_char)
{
  unsigned int cp, w;
  const int alpha_cnt = coll->w_count;
  const unsigned int *weight_ptr = coll->weights;

  assert (coll->uca_exp_num == 0);
  assert (str_size > 0);
  assert (next_char != NULL);

  cp = intl_utf8_to_cp (str, str_size, next_char);
  if (cp < (unsigned int) alpha_cnt)
    {
      COLL_CONTRACTION *contr = NULL;

      if (coll->count_contr > 0
	  && str_size >= coll->contr_min_size
	  && cp >= coll->cp_first_contr_offset
	  && cp < (coll->cp_first_contr_offset
		   + coll->cp_first_contr_count)
	  && ((contr = lang_get_contr_for_string (coll, str, str_size, cp))
	      != NULL))
	{
	  assert (contr != NULL);

	  w = contr->wv;
	  *next_char = (unsigned char *) str + contr->size;
	}
      else
	{
	  w = weight_ptr[cp];
	}
    }
  else
    {
      w = cp;
    }

  return w;
}

/*
 * lang_get_contr_for_string() - checks if the string starts with a
 *				 contraction
 *
 *   return: contraction pointer or NULL if no contraction is found
 *   coll_data(in): collation data
 *   str(in): buffer to check for contractions
 *   str_size(in): size of buffer (bytes)
 *   cp(in): codepoint of first character in 'str'
 *
 */
static COLL_CONTRACTION *
lang_get_contr_for_string (const COLL_DATA * coll_data,
			   const unsigned char *str, const int str_size,
			   unsigned int cp)
{
  const int *first_contr;
  int contr_id;
  COLL_CONTRACTION *contr;
  int cmp;

  assert (coll_data != NULL);
  assert (coll_data->count_contr > 0);

  assert (str != NULL);
  assert (str_size >= coll_data->contr_min_size);

  first_contr = coll_data->cp_first_contr_array;
  assert (first_contr != NULL);
  contr_id = first_contr[cp - coll_data->cp_first_contr_offset];

  if (contr_id == -1)
    {
      return NULL;
    }

  assert (contr_id >= 0 && contr_id < coll_data->count_contr);
  contr = &(coll_data->contr_list[contr_id]);

  do
    {
      if (contr->size > str_size)
	{
	  cmp = memcmp (contr->c_buf, str, str_size);
	  if (cmp == 0)
	    {
	      cmp = 1;
	    }
	}
      else
	{
	  cmp = memcmp (contr->c_buf, str, contr->size);
	}

      if (cmp >= 0)
	{
	  break;
	}

      assert (cmp < 0);

      contr++;
      contr_id++;

    }
  while (contr_id < coll_data->count_contr);

  if (cmp != 0)
    {
      contr = NULL;
    }

  return contr;
}

static UCA_L13_W uca_l13_max_weight = 0xffffffff;
static UCA_L4_W uca_l4_max_weight = 0xffff;

/*
 * lang_get_uca_w_l13() - returns pointer to array of CEs of first collatable
 *			  element in string (codepoint or contraction) and
 *			  number of CEs in this array
 *   return:
 *   coll_data(in): collation data
 *   use_contractions(in):
 *   str(in): string to get weights for
 *   size(in): size of string (bytes)
 *   uca_w_l13(out): pointer to weight array
 *   num_ce(out): number of Collation Elements
 *   str_next(out): pointer to next collatable element in string
 *   cp_out(out): bit field value : codepoint value, and if contraction is
 *		  found than INTL_MASK_CONTR mask is set (MSB)
 */
static void
lang_get_uca_w_l13 (const COLL_DATA * coll_data, const bool use_contractions,
		    const unsigned char *str, const int size,
		    UCA_L13_W ** uca_w_l13, int *num_ce,
		    unsigned char **str_next, unsigned int *cp_out)
{
  unsigned int cp;
  const int alpha_cnt = coll_data->w_count;
  const int exp_num = coll_data->uca_exp_num;

  assert (size > 0);

  cp = intl_utf8_to_cp (str, size, str_next);

  *cp_out = cp;

  if (cp < (unsigned int) alpha_cnt)
    {
      COLL_CONTRACTION *contr = NULL;

      if (use_contractions && coll_data->count_contr > 0
	  && size >= coll_data->contr_min_size
	  && cp >= coll_data->cp_first_contr_offset
	  && cp < (coll_data->cp_first_contr_offset
		   + coll_data->cp_first_contr_count)
	  && ((contr = lang_get_contr_for_string (coll_data, str, size, cp))
	      != NULL))
	{
	  assert (contr != NULL);
	  *uca_w_l13 = contr->uca_w_l13;
	  *num_ce = contr->uca_num;
	  *str_next = (unsigned char *) str + contr->size;
	  *cp_out = INTL_MASK_CONTR | cp;
	}
      else
	{
	  *uca_w_l13 = &(coll_data->uca_w_l13[cp * exp_num]);
	  *num_ce = coll_data->uca_num[cp];
	  /* leave next pointer to the one returned by 'intl_utf8_to_cp' */
	}
    }
  else
    {
      *uca_w_l13 = &uca_l13_max_weight;
      *num_ce = 1;
      /* leave next pointer to the one returned by 'intl_utf8_to_cp' */
    }
}


/*
 * lang_get_uca_back_weight_l13() - returns pointer to array of CEs of
 *				    previous collatable element in string and
 *				    number of CEs in this array
 *
 *   return:
 *   coll_data(in): collation data
 *   use_contractions(in):
 *   str(in): string to get weights for
 *   size(in): size of string (bytes)
 *   uca_w_l13(out): pointer to weight array
 *   num_ce(out): number of Collation Elements
 *   str_next(out): pointer to next collatable element in string
 *   cp_out(out): bit field value : codepoint value, and if contraction is
 *		  found than INTL_MASK_CONTR mask is set (MSB)
 */
static void
lang_get_uca_back_weight_l13 (const COLL_DATA * coll_data,
			      const bool use_contractions,
			      const unsigned char *str_start,
			      const unsigned char *str_last,
			      UCA_L13_W ** uca_w_l13, int *num_ce,
			      unsigned char **str_prev, unsigned int *cp_out)
{
  unsigned int cp;
  const int alpha_cnt = coll_data->w_count;
  const int exp_num = coll_data->uca_exp_num;

  assert (str_prev != NULL);
  assert (cp_out != NULL);
  assert (str_start <= str_last);

  cp = intl_back_utf8_to_cp (str_start, str_last, str_prev);
  *cp_out = cp;

  if (cp < (unsigned int) alpha_cnt)
    {
      COLL_CONTRACTION *contr = NULL;
      unsigned int cp_prev;
      unsigned char *str_prev_prev = NULL;

      if (*str_prev >= str_start)
	{
	  cp_prev = intl_back_utf8_to_cp (str_start, *str_prev,
					  &str_prev_prev);

	  if (use_contractions && coll_data->count_contr > 0
	      && cp_prev < (unsigned int) alpha_cnt
	      && str_last - *str_prev >= coll_data->contr_min_size
	      && cp >= coll_data->cp_first_contr_offset
	      && cp < (coll_data->cp_first_contr_offset
		       + coll_data->cp_first_contr_count)
	      &&
	      ((contr =
		lang_get_contr_for_string (coll_data, str_prev_prev + 1,
					   str_last - str_prev_prev,
					   cp_prev)) != NULL))
	    {
	      assert (contr != NULL);
	      *uca_w_l13 = contr->uca_w_l13;
	      *num_ce = contr->uca_num;
	      *str_prev = str_prev_prev;
	      *cp_out = INTL_MASK_CONTR | cp_prev;
	      return;
	    }
	}

      *uca_w_l13 = &(coll_data->uca_w_l13[cp * exp_num]);
      *num_ce = coll_data->uca_num[cp];
      /* leave str_prev pointer to the one returned by intl_back_utf8_to_cp */
    }
  else
    {
      *uca_w_l13 = &uca_l13_max_weight;
      *num_ce = 1;
      /* leave str_prev pointer to the one returned by 'intl_back_utf8_to_cp' */
    }
}

/*
 * lang_get_uca_w_l4() - returns pointer to array of CEs of first collatable
 *			 element in string (codepoint or contraction) and
 *			 number of CEs in this array
 *   return:
 *   coll_data(in): collation data
 *   use_contractions(in):
 *   str(in): string to get weights for
 *   size(in): size of string (bytes)
 *   uca_w_l13(out): pointer to weight array
 *   num_ce(out): number of Collation Elements
 *   str_next(out): pointer to next collatable element in string
 *   cp_out(out): bit field value : codepoint value, and if contraction is
 *		  found than INTL_MASK_CONTR mask is set (MSB)
 *
 */
static void
lang_get_uca_w_l4 (const COLL_DATA * coll_data, const bool use_contractions,
		   const unsigned char *str, const int size,
		   UCA_L4_W ** uca_w_l4, int *num_ce,
		   unsigned char **str_next, unsigned int *cp_out)
{
  unsigned int cp;
  const int alpha_cnt = coll_data->w_count;
  const int exp_num = coll_data->uca_exp_num;

  assert (size > 0);

  cp = intl_utf8_to_cp (str, size, str_next);

  if (cp < (unsigned int) alpha_cnt)
    {
      COLL_CONTRACTION *contr = NULL;

      if (use_contractions && coll_data->count_contr > 0
	  && size >= coll_data->contr_min_size
	  && cp >= coll_data->cp_first_contr_offset
	  && cp < (coll_data->cp_first_contr_offset
		   + coll_data->cp_first_contr_count)
	  && ((contr = lang_get_contr_for_string (coll_data, str, size, cp))
	      != NULL))
	{
	  assert (contr != NULL);
	  *uca_w_l4 = contr->uca_w_l4;
	  *num_ce = contr->uca_num;
	  *str_next = (unsigned char *) str + contr->size;
	  *cp_out = INTL_MASK_CONTR | cp;
	}
      else
	{
	  *uca_w_l4 = &(coll_data->uca_w_l4[cp * exp_num]);
	  *num_ce = coll_data->uca_num[cp];
	  /* leave next pointer to the one returned by 'intl_utf8_to_cp' */
	}
    }
  else
    {
      *uca_w_l4 = &uca_l4_max_weight;
      *num_ce = 1;
      /* leave next pointer to the one returned by 'intl_utf8_to_cp' */
    }
}

/* retrieve UCA weight level:
 * l = level
 * i = position weight array
 * l13w = array of compressed weight for levels 1,2,3
 * l4w = array of weight level 4
 */
#define GET_UCA_WEIGHT(l, i, l13w, l4w)		\
  ((l == 0) ? (UCA_GET_L1_W (l13w[i])) :	\
   (l == 1) ? (UCA_GET_L2_W (l13w[i])) :	\
   (l == 2) ? (UCA_GET_L3_W (l13w[i])) : (l4w[i]))

#define INTL_CONTR_FOUND(v) (((v) & INTL_MASK_CONTR) == INTL_MASK_CONTR)
/*
 * lang_strmatch_utf8_uca_w_level() - string match or compare for UTF8
 *	collation employing full UCA weights (expansions and contractions)
 *
 *   return: negative if str1 < str2, positive if str1 > str2, zero otherwise
 *   coll_data(in) : collation data
 *   level(in) : current UCA level to compare
 *   is_match(in) : true if match, otherwise is compare
 *   str1(in):
 *   size1(in):
 *   str2(in): this is the pattern string in case of match
 *   size2(in):
 *   escape(in): pointer to escape character (multi-byte allowed)
 *		 (used in context of LIKE)
 *   has_last_escape(in): true if it should check if last character is the
 *			  escape character
 *   offset_next_level(in/out) : offset in bytes from which to start the
 *				 compare; used to avoid compare between
 *				 binary identical part in consecutive compare
 *				 levels
 *   str1_match_size(out): size from str1 which is matched with str2
 */
static int
lang_strmatch_utf8_uca_w_level (const COLL_DATA * coll_data, const int level,
				bool is_match,
				const unsigned char *str1, const int size1,
				const unsigned char *str2, const int size2,
				const unsigned char *escape,
				const bool has_last_escape,
				int *offset_next_level, int *str1_match_size)
{
  const unsigned char *str1_end;
  const unsigned char *str2_end;
  const unsigned char *str1_begin;
  unsigned char *str1_next, *str2_next;
  UCA_L13_W *uca_w_l13_1 = NULL;
  UCA_L13_W *uca_w_l13_2 = NULL;
  UCA_L4_W *uca_w_l4_1 = NULL;
  UCA_L4_W *uca_w_l4_2 = NULL;
  int num_ce1 = 0, num_ce2 = 0;
  int ce_index1 = 0, ce_index2 = 0;
  unsigned int w1 = 0, w2 = 0;

  bool compute_offset = false;
  unsigned int str1_cp_contr = 0, str2_cp_contr = 0;
  int cmp_offset = 0;

  int result = 0;

  assert (offset_next_level != NULL && *offset_next_level > -1);
  assert (level >= 0 && level <= 4);

  str1_end = str1 + size1;
  str2_end = str2 + size2;
  str1_begin = str1;

  if (level == 0)
    {
      assert (*offset_next_level == 0);
      compute_offset = true;
    }
  else
    {
      cmp_offset = *offset_next_level;
      if (cmp_offset > 0)
	{
	  assert (cmp_offset <= size1);
	  assert (cmp_offset <= size2);
	  str1 += cmp_offset;
	  str2 += cmp_offset;

	}
      compute_offset = false;
    }

  str1_next = (unsigned char *) str1;
  str2_next = (unsigned char *) str2;

  for (;;)
    {
    read_weights1:
      if (num_ce1 == 0)
	{
	  str1 = str1_next;
	  if (str1 >= str1_end)
	    {
	      goto read_weights2;
	    }

	  if (level == 3)
	    {
	      lang_get_uca_w_l4 (coll_data, true, str1, str1_end - str1,
				 &uca_w_l4_1, &num_ce1, &str1_next,
				 &str1_cp_contr);
	    }
	  else
	    {
	      lang_get_uca_w_l13 (coll_data, true, str1, str1_end - str1,
				  &uca_w_l13_1, &num_ce1, &str1_next,
				  &str1_cp_contr);
	    }
	  assert (num_ce1 > 0);

	  ce_index1 = 0;
	}

    read_weights2:
      if (num_ce2 == 0)
	{
	  int c_size;

	  str2 = str2_next;
	  if (str2 >= str2_end)
	    {
	      goto compare;
	    }

	  if (is_match && escape != NULL
	      && intl_cmp_char (str2, escape, INTL_CODESET_UTF8,
				&c_size) == 0)
	    {
	      if (!(has_last_escape && str2 + c_size >= str2_end))
		{
		  str2 += c_size;
		}
	    }

	  if (level == 3)
	    {
	      lang_get_uca_w_l4 (coll_data, true, str2, str2_end - str2,
				 &uca_w_l4_2, &num_ce2, &str2_next,
				 &str1_cp_contr);
	    }
	  else
	    {
	      lang_get_uca_w_l13 (coll_data, true, str2, str2_end - str2,
				  &uca_w_l13_2, &num_ce2, &str2_next,
				  &str2_cp_contr);
	    }

	  if (is_match
	      && coll_data->uca_opt.sett_match_contr
	      == MATCH_CONTR_BOUND_ALLOW
	      && !INTL_CONTR_FOUND (str2_cp_contr)
	      && INTL_CONTR_FOUND (str1_cp_contr) && ce_index1 == 0
	      && str2_cp_contr == (str1_cp_contr & (~INTL_MASK_CONTR)))
	    {
	      /* re-compute weight of str1 without considering contractions */
	      if (level == 3)
		{
		  lang_get_uca_w_l4 (coll_data, false, str1, str1_end - str1,
				     &uca_w_l4_1, &num_ce1, &str1_next,
				     &str1_cp_contr);
		}
	      else
		{
		  lang_get_uca_w_l13 (coll_data, false, str1, str1_end - str1,
				      &uca_w_l13_1, &num_ce1, &str1_next,
				      &str1_cp_contr);
		}
	      assert (num_ce1 > 0);
	    }

	  assert (num_ce2 > 0);

	  ce_index2 = 0;
	}

      if (compute_offset)
	{
	  if (ce_index1 == 0 && ce_index2 == 0)
	    {
	      if (!INTL_CONTR_FOUND (str1_cp_contr) &&
		  str1_cp_contr == str2_cp_contr)
		{
		  assert (!INTL_CONTR_FOUND (str2_cp_contr));
		  cmp_offset += str1_next - str1;
		}
	      else
		{
		  compute_offset = false;
		}
	    }
	  else if (ce_index1 != ce_index2)
	    {
	      compute_offset = false;
	    }
	}

    compare:
      if (num_ce1 == 0 && str1 >= str1_end)
	{
	  /* str1 was consumed */
	  if (num_ce2 == 0)
	    {
	      if (str2 >= str2_end)
		{
		  /* both strings consumed and equal */
		  assert (result == 0);
		  goto exit;
		}
	      else
		{
		  if (is_match)
		    {
		      result = -1;
		      goto exit;
		    }
		  goto read_weights2;
		}
	    }

	  assert (num_ce2 > 0);
	  if (is_match && *str2 == 0x20)
	    {
	      /* trailing spaces are not matched */
	      result = -1;
	      goto exit;
	    }

	  /* consume any remaining zero-weight values (skip them) from str2 */
	  do
	    {
	      w2 = GET_UCA_WEIGHT (level, ce_index2, uca_w_l13_2, uca_w_l4_2);
	      if (w2 != 0)
		{
		  /* non-zero weight : strings are not equal */
		  result = -1;
		  goto exit;
		}
	      ce_index2++;
	      num_ce2--;
	    }
	  while (num_ce2 > 0);

	  goto read_weights2;
	}

      if (num_ce2 == 0 && str2 >= str2_end)
	{
	  if (is_match)
	    {
	      assert (result == 0);
	      goto exit;
	    }
	  /* consume any remaining zero-weight values (skip them) from str1 */
	  while (num_ce1 > 0)
	    {
	      w1 = GET_UCA_WEIGHT (level, ce_index1, uca_w_l13_1, uca_w_l4_1);
	      if (w1 != 0)
		{
		  /* non-zero weight : strings are not equal */
		  result = 1;
		  goto exit;
		}
	      ce_index1++;
	      num_ce1--;
	    }

	  goto read_weights1;
	}

      w1 = GET_UCA_WEIGHT (level, ce_index1, uca_w_l13_1, uca_w_l4_1);
      w2 = GET_UCA_WEIGHT (level, ce_index2, uca_w_l13_2, uca_w_l4_2);

      /* ignore zero weights (unless character is space) */
      if (w1 == 0 && *str1 != 0x20)
	{
	  ce_index1++;
	  num_ce1--;

	  if (w2 == 0 && *str2 != 0x20)
	    {
	      ce_index2++;
	      num_ce2--;
	    }

	  goto read_weights1;
	}
      else if (w2 == 0 && *str2 != 0x20)
	{
	  ce_index2++;
	  num_ce2--;

	  goto read_weights1;
	}
      else if (w1 > w2)
	{
	  result = 1;
	  goto exit;
	}
      else if (w1 < w2)
	{
	  result = -1;
	  goto exit;
	}

      ce_index1++;
      ce_index2++;

      num_ce1--;
      num_ce2--;
    }

  if (str1_end - str1 > 0 || str2_end - str2 > 0)
    {
      result = lang_strcmp_check_trail_spaces (str1, str1_end - str1,
					       str2, str2_end - str2);
      goto exit;
    }
  else
    {
      if (num_ce1 > num_ce2)
	{
	  result = 1;
	  goto exit;
	}
      else if (num_ce1 < num_ce2)
	{
	  result = -1;
	  goto exit;
	}
    }

exit:
  if (is_match)
    {
      assert (str1_match_size != NULL);
      *str1_match_size = str1 - str1_begin;
    }

  if (level == 0)
    {
      *offset_next_level = cmp_offset;
    }
  return result;
}

/*
 * lang_back_strmatch_utf8_uca_w_level() - string match or compare for UTF8
 *	collation employing full UCA weights (expansions and contractions)
 *
 *   return: negative if str1 < str2, positive if str1 > str2, zero otherwise
 *   coll_data(in) : collation data
 *   level(in) : current UCA level to compare
 *   is_match(in) : true if match, otherwise is compare
 *   str1(in):
 *   size1(in):
 *   str2(in): this is the pattern string in case of match
 *   size2(in):
 *   escape(in): pointer to escape character (multi-byte allowed)
 *		 (used in context of LIKE)
 *   has_last_escape(in): true if it should check if last character is the
 *			  escape character
 *   offset_next_level(in/out) : offset in bytes from which to start the
 *				 compare; used to avoid compare between
 *				 binary identical part in consecutive compare
 *				 levels
 *   str1_match_size(out): size from str1 which is matched with str2
 */
static int
lang_back_strmatch_utf8_uca_w_level (const COLL_DATA * coll_data,
				     bool is_match,
				     const unsigned char *str1,
				     const int size1,
				     const unsigned char *str2,
				     const int size2,
				     const unsigned char *escape,
				     const bool has_last_escape,
				     int *offset_next_level,
				     int *str1_match_size)
{
  const unsigned char *str1_start;
  const unsigned char *str2_start;
  const unsigned char *str1_last;
  const unsigned char *str2_last;
  unsigned char *str1_prev, *str2_prev;
  UCA_L13_W *uca_w_l13_1 = NULL;
  UCA_L13_W *uca_w_l13_2 = NULL;
  int num_ce1 = 0, num_ce2 = 0;
  int ce_index1 = -1, ce_index2 = -1;
  unsigned int w1 = 0, w2 = 0;
  unsigned int str1_cp_contr = 0, str2_cp_contr = 0;
  int result = 0;

  assert (offset_next_level != NULL && *offset_next_level > -1);

  str1_last = str1 + size1 - 1;
  str2_last = str2 + size2 - 1;
  str1_start = str1;
  str2_start = str2;

  while (*str1_last == 0x20)
    {
      str1_last--;
    }

  while (*str2_last == 0x20)
    {
      str2_last--;
    }

  str1_prev = (unsigned char *) str1_last;
  str2_prev = (unsigned char *) str2_last;

  for (;;)
    {
    read_weights1:
      if (ce_index1 < 0)
	{
	  str1 = str1_prev;
	  if (str1 < str1_start)
	    {
	      goto read_weights2;
	    }

	  lang_get_uca_back_weight_l13 (coll_data, true, str1_start, str1,
					&uca_w_l13_1, &num_ce1, &str1_prev,
					&str1_cp_contr);

	  assert (num_ce1 > 0);

	  ce_index1 = num_ce1 - 1;
	}

    read_weights2:
      if (ce_index2 < 0)
	{
	  int c_size;

	  str2 = str2_prev;
	  if (str2 < str2_start)
	    {
	      goto compare;
	    }

	  if (is_match && escape != NULL
	      && !(has_last_escape && str2 == str2_last))
	    {
	      unsigned char *str2_prev_prev;

	      (void) intl_back_utf8_to_cp (str2, str2_start, &str2_prev_prev);

	      if (intl_cmp_char
		  (str2_prev_prev + 1, escape, INTL_CODESET_UTF8,
		   &c_size) == 0)
		{
		  str2 = str2_prev_prev;
		}
	    }

	  lang_get_uca_back_weight_l13 (coll_data, true, str2_start, str2,
					&uca_w_l13_2, &num_ce2, &str2_prev,
					&str2_cp_contr);

	  assert (num_ce2 > 0);

	  ce_index2 = num_ce2 - 1;

	  if (is_match
	      && coll_data->uca_opt.sett_match_contr
	      == MATCH_CONTR_BOUND_ALLOW
	      && !INTL_CONTR_FOUND (str2_cp_contr)
	      && INTL_CONTR_FOUND (str1_cp_contr) && ce_index1 == num_ce1 - 1
	      && str2_cp_contr == (str1_cp_contr & (~INTL_MASK_CONTR)))
	    {
	      /* re-compute weight of str1 without considering contractions */
	      lang_get_uca_back_weight_l13 (coll_data, false, str1_start,
					    str1, &uca_w_l13_1, &num_ce1,
					    &str1_prev, &str1_cp_contr);

	      assert (num_ce1 > 0);
	      ce_index1 = num_ce1 - 1;
	    }
	}

    compare:
      if (ce_index1 < 0 && str1 < str1_start)
	{
	  /* str1 was consumed */
	  if (ce_index2 < 0)
	    {
	      if (str2 < str2_start)
		{
		  /* both strings consumed and equal */
		  assert (result == 0);
		  goto exit;
		}
	      else
		{
		  if (is_match)
		    {
		      result = -1;
		      goto exit;
		    }
		  goto read_weights2;
		}
	    }

	  assert (ce_index2 >= 0);
	  if (is_match)
	    {
	      /* trailing spaces are not matched */
	      result = -1;
	      goto exit;
	    }

	  /* consume any remaining zero-weight values (skip them) from str2 */
	  do
	    {
	      w2 = UCA_GET_L2_W (uca_w_l13_2[ce_index2]);
	      if (w2 != 0)
		{
		  /* non-zero weight : strings are not equal */
		  result = -1;
		  goto exit;
		}
	      ce_index2--;
	    }
	  while (ce_index2 > 0);

	  goto read_weights2;
	}

      if (ce_index2 < 0 && str2 < str2_start)
	{
	  if (is_match)
	    {
	      assert (result == 0);
	      goto exit;
	    }
	  /* consume any remaining zero-weight values (skip them) from str1 */
	  while (ce_index1 >= 0)
	    {
	      w1 = UCA_GET_L2_W (uca_w_l13_1[ce_index1]);
	      if (w1 != 0)
		{
		  /* non-zero weight : strings are not equal */
		  result = 1;
		  goto exit;
		}
	      ce_index1--;
	    }

	  goto read_weights1;
	}

      assert (ce_index1 >= 0 && ce_index2 >= 0);

      w1 = UCA_GET_L2_W (uca_w_l13_1[ce_index1]);
      w2 = UCA_GET_L2_W (uca_w_l13_2[ce_index2]);

      /* ignore zero weights (unless character is space) */
      if (w1 == 0 && *str1 != 0x20)
	{
	  ce_index1--;

	  if (w2 == 0 && *str2 != 0x20)
	    {
	      ce_index2--;
	    }

	  goto read_weights1;
	}
      else if (w2 == 0 && *str2 != 0x20)
	{
	  ce_index2--;
	  goto read_weights1;
	}
      else if (w1 > w2)
	{
	  result = 1;
	  goto exit;
	}
      else if (w1 < w2)
	{
	  result = -1;
	  goto exit;
	}

      ce_index1--;
      ce_index2--;
    }

  if (str1 > str1_start)
    {
      assert (str2 <= str2_start);
      result = 1;
    }
  else if (str2 > str2_start)
    {
      assert (str1 <= str1_start);
      result = -1;
    }
  else
    {
      if (ce_index1 > ce_index2)
	{
	  result = 1;
	  goto exit;
	}
      else if (ce_index1 < ce_index2)
	{
	  result = -1;
	  goto exit;
	}
    }

exit:
  if (is_match)
    {
      assert (str1_match_size != NULL);
      *str1_match_size = str1_last - str1_start + 1;
    }

  return result;
}

/*
 * lang_strcmp_utf8_uca() - string compare for UTF8 for a collation using
 *			    full UCA weights (expansions and contractions)
 *   return:
 *   lang_coll(in):
 *   string1(in):
 *   size1(in):
 *   string2(in):
 *   size2(in):
 */
static int
lang_strcmp_utf8_uca (const LANG_COLLATION * lang_coll,
		      const unsigned char *str1, const int size1,
		      const unsigned char *str2, const int size2)
{
  return lang_strmatch_utf8_uca_w_coll_data (&(lang_coll->coll), false,
					     str1, size1, str2, size2, NULL,
					     false, NULL);
}

/*
 * lang_strmatch_utf8_uca() - string match for UTF8 for a collation using
 *			      full UCA weights (expansions and contractions)
 *   return:
 *   lang_coll(in):
 *   is_match(in):
 *   string1(in):
 *   size1(in):
 *   string2(in):
 *   size2(in):
 *   escape(in):
 *   has_last_escape(in):
 *   str1_match_size(out):
 */
static int
lang_strmatch_utf8_uca (const LANG_COLLATION * lang_coll, bool is_match,
			const unsigned char *str1, const int size1,
			const unsigned char *str2, const int size2,
			const unsigned char *escape,
			const bool has_last_escape, int *str1_match_size)
{
  return lang_strmatch_utf8_uca_w_coll_data (&(lang_coll->coll), is_match,
					     str1, size1, str2, size2, escape,
					     has_last_escape,
					     str1_match_size);
}

/*
 * lang_strmatch_utf8_uca_w_coll_data() - string match/compare for UTF8 for a
 *   collation using full UCA weights (+ expansions and contractions)
 *
 *   return: negative if str1 < str2, positive if str1 > str2, zero otherwise
 *   coll_data(in):
 *   is_match(in) : true if match, otherwise is compare
 *   str1(in):
 *   size1(in):
 *   str2(in): this is the pattern string in case of match
 *   size2(in):
 *   escape(in): pointer to escape character (multi-byte allowed)
 *		 (used in context of LIKE)
 *   has_last_escape(in): true if it should check if last character is the
 *			  escape character
 *   str1_match_size(out): size from str1 which is matched with str2
 */
int
lang_strmatch_utf8_uca_w_coll_data (const COLL_DATA * coll_data,
				    bool is_match, const unsigned char *str1,
				    const int size1,
				    const unsigned char *str2,
				    const int size2,
				    const unsigned char *escape,
				    const bool has_last_escape,
				    int *str1_match_size)
{
  int res;
  int cmp_offset = 0;

  /* compare level 1 */
  res = lang_strmatch_utf8_uca_w_level (coll_data, 0, is_match, str1, size1,
					str2, size2, escape, has_last_escape,
					&cmp_offset, str1_match_size);
  if (res != 0)
    {
      return res;
    }

  if (coll_data->uca_opt.sett_strength == TAILOR_PRIMARY)
    {
      if (coll_data->uca_opt.sett_caseLevel)
	{
	  /* compare level 3 (casing) */
	  res = lang_strmatch_utf8_uca_w_level (coll_data, 2, is_match,
						str1, size1, str2, size2,
						escape, has_last_escape,
						&cmp_offset, str1_match_size);
	  if (res != 0)
	    {
	      /* reverse order when caseFirst == UPPER */
	      return (coll_data->uca_opt.sett_caseFirst == 1) ? -res : res;
	    }
	}
      return 0;
    }

  assert (coll_data->uca_opt.sett_strength >= TAILOR_SECONDARY);

  /* compare level 2 */
  if (coll_data->uca_opt.sett_backwards)
    {
      int str1_level_2_size;

      if (is_match)
	{
	  str1_level_2_size = *str1_match_size;
	}
      else
	{
	  str1_level_2_size = size1;
	}
      if (str1_level_2_size > 0 && size2 > 0)
	{
	  res = lang_back_strmatch_utf8_uca_w_level (coll_data, is_match,
						     str1, str1_level_2_size,
						     str2, size2,
						     escape, has_last_escape,
						     &cmp_offset,
						     str1_match_size);
	}
      else
	{
	  res = (str1_level_2_size == size2)
	    ? 0 : ((str1_level_2_size > size2) ? 1 : -1);
	}
    }
  else
    {
      res = lang_strmatch_utf8_uca_w_level (coll_data, 1, is_match,
					    str1, size1, str2, size2,
					    escape, has_last_escape,
					    &cmp_offset, str1_match_size);
    }

  if (res != 0)
    {
      return res;
    }

  if (coll_data->uca_opt.sett_strength == TAILOR_SECONDARY)
    {
      return 0;
    }

  /* compare level 3 */
  res = lang_strmatch_utf8_uca_w_level (coll_data, 2, is_match, str1, size1,
					str2, size2,
					escape, has_last_escape,
					&cmp_offset, str1_match_size);
  if (res != 0)
    {
      /* reverse order when caseFirst == UPPER */
      return (coll_data->uca_opt.sett_caseFirst == 1) ? -res : res;
    }

  if (coll_data->uca_opt.sett_strength == TAILOR_TERTIARY)
    {
      return 0;
    }

  /* compare level 4 */
  res = lang_strmatch_utf8_uca_w_level (coll_data, 3, is_match, str1, size1,
					str2, size2,
					escape, has_last_escape,
					&cmp_offset, str1_match_size);
  if (res != 0)
    {
      /* reverse order when caseFirst == UPPER */
      return res;
    }

  return 0;
}

/*
 * lang_strcmp_check_trail_spaces() - string compare the trailing spaces
 *				      of UTF-8 sequence
 *   return:
 *   string1(in):
 *   size1(in):
 *   string2(in):
 *   size2(in):
 */
static int
lang_strcmp_check_trail_spaces (const unsigned char *str1, const int size1,
				const unsigned char *str2, const int size2)
{
  if (size1 == size2)
    {
      assert (size1 == 0);
      return 0;
    }
  else if (size1 < size2)
    {
      const unsigned char *str2_end = str2 + size2;

      for (; str2 < str2_end;)
	{
	  /* ignore trailing white spaces */
	  if (*str2 != 32 && *str2 != 0)
	    {
	      return -1;
	    }
	  str2 += intl_Len_utf8_char[*str2];
	}
    }
  else
    {
      const unsigned char *str1_end = str1 + size1;

      for (; str1 < str1_end;)
	{
	  /* ignore trailing white spaces */
	  if (*str1 != 32 && *str1 != 0)
	    {
	      return 1;
	    }
	  str1 += intl_Len_utf8_char[*str1];
	}
    }

  return 0;
}

/*
 * lang_next_coll_char_utf8() - computes the next collatable char
 *   return: size in bytes of the next collatable char
 *   lang_coll(on): collation
 *   seq(in): pointer to current char
 *   size(in): available bytes for current char
 *   next_seq(in/out): buffer to return next alphabetical char
 *   len_next(in/out): length in chars of next char (always 1 for this func)
 *
 *  Note :  It is assumed that the input buffer (cur_char) contains at least
 *	    one UTF-8 character.
 *	    The calling function should take into account cases when 'next'
 *	    character is encoded on greater byte size.
 */
static int
lang_next_coll_char_utf8 (const LANG_COLLATION * lang_coll,
			  const unsigned char *seq, const int size,
			  unsigned char *next_seq, int *len_next)
{
  unsigned int cp_alpha_char, cp_next_alpha_char;
  const int alpha_cnt = lang_coll->coll.w_count;
  const unsigned int *next_alpha_char = lang_coll->coll.next_cp;
  unsigned char *dummy = NULL;

  assert (seq != NULL);
  assert (next_seq != NULL);
  assert (len_next != NULL);
  assert (size > 0);

  cp_alpha_char = intl_utf8_to_cp (seq, size, &dummy);

  if (cp_alpha_char < (unsigned int) alpha_cnt)
    {
      cp_next_alpha_char = next_alpha_char[cp_alpha_char];
    }
  else
    {
      cp_next_alpha_char = cp_alpha_char + 1;
    }

  *len_next = 1;

  return intl_cp_to_utf8 (cp_next_alpha_char, next_seq);
}

/*
 * lang_next_coll_seq_utf8_w_contr() - computes the next collatable sequence
 *				       for locales having contractions
 *   return: size in bytes of the next collatable sequence
 *   lang_coll(on): collation
 *   seq(in): pointer to current sequence
 *   size(in): available bytes for current sequence
 *   next_seq(in/out): buffer to return next collatable sequence
 *   len_next(in/out): length in chars of next sequence
 *
 *  Note :  It is assumed that the input buffer (cur_char) contains at least
 *	    one UTF-8 character.
 */
static int
lang_next_coll_seq_utf8_w_contr (const LANG_COLLATION * lang_coll,
				 const unsigned char *seq, const int size,
				 unsigned char *next_seq, int *len_next)
{
  unsigned int cp_first_char;
  unsigned int next_seq_id;
  unsigned int cp_next_char;
  const int alpha_cnt = lang_coll->coll.w_count;
  const unsigned int *next_alpha_char = lang_coll->coll.next_cp;
  unsigned char *dummy = NULL;
  COLL_CONTRACTION *contr = NULL;

  assert (seq != NULL);
  assert (next_seq != NULL);
  assert (len_next != NULL);
  assert (size > 0);

  cp_first_char = intl_utf8_to_cp (seq, size, &dummy);

  if (cp_first_char < (unsigned int) alpha_cnt)
    {
      if (size >= lang_coll->coll.contr_min_size
	  && cp_first_char >= lang_coll->coll.cp_first_contr_offset
	  && cp_first_char < (lang_coll->coll.cp_first_contr_offset
			      + lang_coll->coll.cp_first_contr_count))
	{
	  contr = lang_get_contr_for_string (&(lang_coll->coll), seq, size,
					     cp_first_char);
	}

      if (contr == NULL)
	{
	  next_seq_id = next_alpha_char[cp_first_char];
	}
      else
	{
	  next_seq_id = contr->next;
	}

      if (INTL_IS_NEXT_CONTR (next_seq_id))
	{
	  contr =
	    &(lang_coll->coll.
	      contr_list[INTL_GET_NEXT_CONTR_ID (next_seq_id)]);
	  memcpy (next_seq, contr->c_buf, contr->size);
	  *len_next = contr->cp_count;
	  return contr->cp_count;
	}
      else
	{
	  cp_next_char = next_seq_id;
	}
    }
  else
    {
      /* codepoint is not collated in current locale */
      cp_next_char = cp_first_char + 1;
    }

  *len_next = 1;
  return intl_cp_to_utf8 (cp_next_char, next_seq);
}

/*
 * lang_split_key_iso() - finds the prefix key between two strings (ISO
 *			  charset with cases sensitive collation and bit
 *			  strings)
 *
 *   return:  error status
 *   lang_coll(in):
 *   is_desc(in):
 *   str1(in):
 *   size1(in):
 *   str2(in):
 *   size2(in):
 *   key(out): key
 *   byte_size(out): size in bytes of key
 *
 *  Note : this function is used by index prefix computation (BTREE building)
 */
static int
lang_split_key_iso (const LANG_COLLATION * lang_coll, const bool is_desc,
		    const unsigned char *str1, const int size1,
		    const unsigned char *str2, const int size2,
		    unsigned char **key, int *byte_size)
{
  int pos;
  const unsigned char *t, *t2;

  assert (key != NULL);
  assert (byte_size != NULL);

  for (pos = 0, t = str1, t2 = str2;
       pos < size1 && pos < size2 && *t++ == *t2++; pos++)
    {
      ;
    }

  if (!is_desc)
    {				/* normal index */
      /* check if matched size is exactly string1 or in string2 there is
       * exactly one more character (byte) after matched part */
      if (pos == size1 || pos + 1 == size2)
	{
	  *key = (unsigned char *) str1;
	  pos = size1;
	}
      else
	{
	  assert (pos < size2);
	  assert (pos < size1);

	  *key = (unsigned char *) str2;
	  assert (*(str2 + pos) != 0 && *(str2 + pos) != 0x20);
	  pos++;
	}
    }
  else
    {				/* reverse index */
      assert (is_desc);

      if (pos == size1)
	{
	  /* actually, this could happen only when string1 == string2 */
	  *key = (unsigned char *) str1;
	  pos = size1;
	}
      else
	{
	  assert (pos < size1);
	  *key = (unsigned char *) str1;
	  /* we must add a non-zero weight character */
	  for (t = str1 + pos; pos < size1 && (*t == 0x20 || *t == 0);
	       t++, pos++)
	    {
	      ;
	    }

	  if (pos < size1)
	    {
	      pos++;
	    }
	}
    }

  *byte_size = pos;

  return NO_ERROR;
}

/*
 * lang_split_key_byte() - finds the prefix key :
 *			   collations  with byte-characters (ISO charset) and
 *			   weight values (e.g. case insensitive).
 *
 *   return:  error status
 *   lang_coll(in):
 *   is_desc(in):
 *   str1(in):
 *   size1(in):
 *   str2(in):
 *   size2(in):
 *   key(out): key
 *   byte_size(out): size in bytes of key
 *
 *  Note : this function is used by index prefix computation (BTREE building)
 */
static int
lang_split_key_byte (const LANG_COLLATION * lang_coll, const bool is_desc,
		     const unsigned char *str1, const int size1,
		     const unsigned char *str2, const int size2,
		     unsigned char **key, int *byte_size)
{
  int pos, cmp;
  const unsigned char *t1, *t2;

  assert (key != NULL);
  assert (byte_size != NULL);

  for (pos = 0, t1 = str1, t2 = str2; pos < size1 && pos < size2; pos++)
    {
      cmp = lang_coll->coll.weights[*t1++] - lang_coll->coll.weights[*t2++];

      if (cmp != 0)
	{
	  assert ((!is_desc && cmp < 0) || (is_desc && cmp > 0));
	  break;
	}
    }

  if (!is_desc)
    {				/* normal index */
      if (pos == size1 || pos + 1 == size2)
	{
	  *key = (unsigned char *) str1;
	  pos = size1;
	}
      else
	{
	  assert (pos < size2);
	  assert (pos < size1);

	  *key = (unsigned char *) str2;
	  assert (lang_coll->coll.weights[*(str2 + pos)] != 0);
	  pos++;
	}
    }
  else
    {				/* reverse index */
      if (pos == size1)
	{
	  *key = (unsigned char *) str1;
	  pos = size1;
	}
      else
	{
	  assert (pos < size1);
	  *key = (unsigned char *) str1;
	  /* we must add a non-zero weight character */
	  for (t1 = str1 + pos;
	       pos < size1 && lang_coll->coll.weights[*t1] == 0; t1++, pos++)
	    {
	      ;
	    }

	  if (pos < size1)
	    {
	      pos++;
	    }
	}
    }

  *byte_size = pos;

  return NO_ERROR;
}

/*
 * lang_split_key_utf8() - finds the prefix key; UTF-8 collation with
 *			   contractions but without expansions
 *
 *   return:  error status
 *   lang_coll(in):
 *   is_desc(in):
 *   str1(in):
 *   size1(in):
 *   str2(in):
 *   size2(in):
 *   key(out): key
 *   byte_size(out): size in bytes of key
 *
 *  Note : this function is used by index prefix computation (BTREE building)
 */
static int
lang_split_key_utf8 (const LANG_COLLATION * lang_coll, const bool is_desc,
		     const unsigned char *str1, const int size1,
		     const unsigned char *str2, const int size2,
		     unsigned char **key, int *byte_size)
{
  const unsigned char *str1_end, *str2_end;
  const unsigned char *str1_begin, *str2_begin;
  unsigned char *str1_next, *str2_next;
  unsigned int w1, w2;
  int key_size;
  const COLL_DATA *coll = &(lang_coll->coll);

  assert (key != NULL);
  assert (byte_size != NULL);

  str1_end = str1 + size1;
  str2_end = str2 + size2;
  str1_begin = str1;
  str2_begin = str2;

  for (; str1 < str1_end && str2 < str2_end;)
    {
      w1 = lang_get_w_first_el (coll, str1, str1_end - str1, &str1_next);
      w2 = lang_get_w_first_el (coll, str2, str2_end - str2, &str2_next);

      if (w1 != w2)
	{
	  assert ((!is_desc && w1 < w2) || (is_desc && w1 > w2));
	  break;
	}

      str1 = str1_next;
      str2 = str2_next;
    }

  if (!is_desc)
    {				/* normal index */
      bool string2_has_one_more_char = false;

      if (str1 < str1_end)
	{
	  /* check if in string2 there is one more character (or contraction)
	   * after common part */
	  if (str2 < str2_end)
	    {
	      w2 = lang_get_w_first_el (coll, str2, str2_end - str2,
					&str2_next);
	      assert (w2 != 0);
	      if (str2_next == str2_end)
		{
		  string2_has_one_more_char = true;
		}
	    }
	}

      if (str1 == str1_end || string2_has_one_more_char)
	{
	  *key = (unsigned char *) str1_begin;
	  key_size = size1;
	}
      else
	{
	  assert (str2 < str2_end);

	  *key = (unsigned char *) str2_begin;

	  /* common part plus one more character (or more, if last unit is a
	   * contraction) from string2 */
	  key_size = str2_next - str2_begin;
	  assert (key_size <= size2);
	}
    }
  else
    {				/* reverse index */
      if (str1 == str1_end)
	{
	  /* actually, this could happen only when string1 == string2 */
	  *key = (unsigned char *) str1_begin;
	  key_size = size1;
	}
      else
	{
	  assert (str1 < str1_end);
	  *key = (unsigned char *) str1_begin;

	  /* common part plus one more non-zero weight collation unit */
	  do
	    {
	      w1 = lang_get_w_first_el (coll, str1, str1_end - str1,
					&str1_next);
	      str1 = str1_next;
	    }
	  while (w1 == 0 && str1 < str1_end);

	  key_size = str1_next - str1_begin;

	  assert (key_size <= size1);
	}
    }

  *byte_size = key_size;

  return NO_ERROR;
}

/*
 * lang_split_key_w_exp() - finds the prefix key for UTF-8 strings and
 *			    collation with expansions
 *
 *   return:  error status
 *   lang_coll(in):
 *   is_desc(in):
 *   str1(in):
 *   size1(in):
 *   str2(in):
 *   size2(in):
 *   key(out): key
 *   byte_size(out): size in bytes in key
 *
 *  Note : this function is used by index prefix computation (BTREE building)
 */
static int
lang_split_key_w_exp (const LANG_COLLATION * lang_coll, const bool is_desc,
		      const unsigned char *str1, const int size1,
		      const unsigned char *str2, const int size2,
		      unsigned char **key, int *byte_size)
{
  const unsigned char *str1_end;
  const unsigned char *str2_end;
  unsigned char *str1_next, *str2_next;
  unsigned char *str1_begin, *str2_begin;
  UCA_L13_W *uca_w_l13_1 = NULL;
  UCA_L13_W *uca_w_l13_2 = NULL;
  int num_ce1 = 0, num_ce2 = 0;
  int ce_index1 = 0, ce_index2 = 0;
  unsigned int w1 = 0, w2 = 0;
  const COLL_DATA *cd = &(lang_coll->coll);
  int dummy, key_size;
  int last_str1_unit_size = 1, last_str2_unit_size = 1;
  bool force_key = false;

  assert (key != NULL);
  assert (byte_size != NULL);

  str1_begin = str1_next = (unsigned char *) str1;
  str2_begin = str2_next = (unsigned char *) str2;

  str1_end = str1 + size1;
  str2_end = str2 + size2;

  /* Regular string compare in collation with expansions requires multiple
   * passes up to the UCA level of collation or until a weight difference
   * Key prefix algorithm takes into account only level 1 of weight */
  for (;;)
    {
    read_weights1:
      if (num_ce1 == 0)
	{
	  str1 = str1_next;
	  if (str1 >= str1_end)
	    {
	      goto read_weights2;
	    }

	  lang_get_uca_w_l13 (cd, true, str1, str1_end - str1,
			      &uca_w_l13_1, &num_ce1, &str1_next, &dummy);
	  assert (num_ce1 > 0);

	  ce_index1 = 0;

	  last_str1_unit_size = str1_next - str1;
	}

    read_weights2:
      if (num_ce2 == 0)
	{
	  str2 = str2_next;
	  if (str2 >= str2_end)
	    {
	      goto compare;
	    }

	  lang_get_uca_w_l13 (cd, true, str2, str2_end - str2,
			      &uca_w_l13_2, &num_ce2, &str2_next, &dummy);

	  assert (num_ce2 > 0);

	  ce_index2 = 0;

	  last_str2_unit_size = str2_next - str2;
	}

    compare:
      if ((num_ce1 == 0 && str1 >= str1_end)
	  || (num_ce2 == 0 && str2 >= str2_end))
	{
	  force_key = true;
	  break;
	}

      w1 = UCA_GET_L1_W (uca_w_l13_1[ce_index1]);
      w2 = UCA_GET_L1_W (uca_w_l13_2[ce_index2]);

      /* ignore zero weights (unless character is space) */
      if (w1 == 0 && *str1 != 0x20)
	{
	  ce_index1++;
	  num_ce1--;

	  if (w2 == 0 && *str2 != 0x20)
	    {
	      ce_index2++;
	      num_ce2--;
	    }

	  goto read_weights1;
	}
      else if (w2 == 0 && *str2 != 0x20)
	{
	  ce_index2++;
	  num_ce2--;

	  goto read_weights1;
	}
      else if (w1 != w2)
	{
	  assert ((is_desc && w1 > w2) || (!is_desc && w1 < w2));
	  break;
	}

      assert (w1 == w2);

      ce_index1++;
      ce_index2++;

      num_ce1--;
      num_ce2--;
    }

  if (force_key)
    {
      *key = str1_begin;
      *byte_size = size1;
      return NO_ERROR;
    }

  if (!is_desc)
    {				/* normal index */
      bool string2_has_one_more_char = false;

      if (str1 < str1_end)
	{
	  /* check if in string2 there is one more character after common
	     part */
	  if (str2 < str2_end)
	    {
	      /* get end of current element, weight should be non-zero */
	      lang_get_uca_w_l13 (cd, true, str2, str2_end - str2,
				  &uca_w_l13_2, &num_ce2, &str2_next, &dummy);
	      assert (UCA_GET_L1_W (uca_w_l13_2[0]) != 0);
	      if (str2_next == str2_end)
		{
		  string2_has_one_more_char = true;
		}
	    }
	}

      if (str1 == str1_end || string2_has_one_more_char)
	{
	  *key = str1_begin;
	  key_size = size1;
	}
      else
	{
	  assert (str1 < str1_end);
	  assert (str2 < str2_end);
	  *key = str2_begin;

	  /* equal part plus one more element (character or contraction)
	   * the end of element was computed above (str2_next) */
	  key_size = str2_next - str2_begin;
	  assert (key_size <= size2);
	}
    }
  else
    {				/* reverse index */
      if (str1 == str1_end)
	{
	  /* this could happen only when string1 == string2 */
	  *key = str1_begin;
	  key_size = size1;
	}
      else
	{
	  assert (str1 < str1_end);
	  *key = str1_begin;

	  /* common part plus one more non-zero weight collation unit */
	  do
	    {
	      lang_get_uca_w_l13 (cd, true, str1, str1_end - str1,
				  &uca_w_l13_1, &num_ce1, &str1_next, &dummy);
	      str1 = str1_next;
	    }
	  while (UCA_GET_L1_W (uca_w_l13_1[0]) == 0 && str1 < str1_end);

	  /* equal part plus one more element (character or contraction) */
	  key_size = str1_next - str1_begin;
	  assert (key_size <= size1);
	}
    }

  *byte_size = key_size;

  return NO_ERROR;
}

/*
 * lang_split_key_euc() - finds the prefix key for EUC-KR collation
 *
 *   return:  error status
 *   lang_coll(in):
 *   is_desc(in):
 *   str1(in):
 *   size1(in):
 *   str2(in):
 *   size2(in):
 *   key(out): key
 *   byte_size(out): size in bytes in key
 *
 *  Note : this function is used by index prefix computation (BTREE building)
 */
static int
lang_split_key_euc (const LANG_COLLATION * lang_coll, const bool is_desc,
		    const unsigned char *str1, const int size1,
		    const unsigned char *str2, const int size2,
		    unsigned char **key, int *byte_size)
{
  unsigned char *str1_next, *str2_next;
  int key_size, char1_size, char2_size;
  const unsigned char *str1_end, *str2_end;
  const unsigned char *str1_begin, *str2_begin;

  assert (key != NULL);
  assert (byte_size != NULL);

  str1_end = str1 + size1;
  str2_end = str2 + size2;
  str1_begin = str1;
  str2_begin = str2;

  for (; str1 < str1_end && str2 < str2_end;)
    {
      str1_next = intl_nextchar_euc ((unsigned char *) str1, &char1_size);
      str2_next = intl_nextchar_euc ((unsigned char *) str2, &char2_size);

      if (char1_size != char2_size || memcmp (str1, str2, char1_size) != 0)
	{
	  break;
	}

      str1 = str1_next;
      str2 = str2_next;
    }

  if (!is_desc)
    {				/* normal index */
      bool string2_has_one_more_char = false;

      if (str1 < str1_end)
	{
	  /* check if in string2 there is one more character after common
	     part */
	  if (str2 < str2_end)
	    {
	      (void) intl_nextchar_euc ((unsigned char *) str2, &char2_size);
	      if (str2 + char2_size == str2_end)
		{
		  string2_has_one_more_char = true;
		}
	    }
	}

      if (str1 == str1_end || string2_has_one_more_char)
	{
	  *key = (unsigned char *) str1_begin;
	  key_size = size1;
	}
      else
	{
	  bool is_zero_weight;
	  assert (str2 < str2_end);

	  /* common part plus one more non-zero weight character */
	  do
	    {
	      str2_next = intl_nextchar_euc ((unsigned char *) str2,
					     &char2_size);
	      is_zero_weight = false;
	      if (*str2 == 0x20 || *str2 == 0
		  || (*str2 == 0xa1 && char2_size == 2
		      && *(str2 + 1) == 0xa1))
		{
		  is_zero_weight = true;
		}

	      if (str2 + char2_size == str2_end)
		{
		  string2_has_one_more_char = true;
		  break;
		}
	      str2 = str2_next;
	    }
	  while (is_zero_weight && str2 < str2_end);

	  if (string2_has_one_more_char)
	    {
	      *key = (unsigned char *) str1_begin;
	      key_size = size1;
	    }
	  else
	    {
	      *key = (unsigned char *) str2_begin;
	      key_size = str2 - str2_begin + char2_size;
	    }
	}
    }
  else
    {				/* reverse index */
      if (str1 == str1_end)
	{
	  /* actually, this could happen only when string1 == string2 */
	  *key = (unsigned char *) str1_begin;
	  key_size = size1;
	}
      else
	{
	  bool is_zero_weight;
	  assert (str1 < str1_end);
	  *key = (unsigned char *) str1_begin;

	  /* common part plus one more non-zero weight character */
	  do
	    {
	      str1_next = intl_nextchar_euc ((unsigned char *) str1,
					     &char1_size);
	      is_zero_weight = false;
	      if (*str1 == 0x20 || *str1 == 0
		  || (*str1 == 0xa1 && char1_size == 2
		      && *(str1 + 1) == 0xa1))
		{
		  is_zero_weight = true;
		}
	      str1 = str1_next;
	    }
	  while (is_zero_weight && str1 < str1_end);

	  key_size = str1 - str1_end + char1_size;
	}
    }

  *byte_size = key_size;

  return NO_ERROR;
}

/*
 * English Locale Data
 */

/* English collation */
static unsigned int lang_upper_EN[LANG_CHAR_COUNT_EN];
static unsigned int lang_lower_EN[LANG_CHAR_COUNT_EN];

#if !defined(LANG_W_MAP_COUNT_EN)
#define	LANG_W_MAP_COUNT_EN 256
#endif
static int lang_w_map_EN[LANG_W_MAP_COUNT_EN];

/*
 * lang_initloc_en () - init locale data for English language
 *   return:
 */
static void
lang_initloc_en_iso88591 (LANG_LOCALE_DATA * ld)
{
  assert (ld != NULL);

  coll_iso_binary.default_lang = ld;
  coll_iso88591_en_cs.default_lang = ld;
  coll_iso88591_en_ci.default_lang = ld;

  ld->is_initialized = true;
}

/*
 * lang_init_common_en_cs () - init collation data for English case
 *			       sensitive (no matter the charset)
 *   return:
 */
static void
lang_init_common_en_cs (void)
{
  int i;
  static bool is_common_en_cs_init = false;

  if (is_common_en_cs_init)
    {
      return;
    }

  for (i = 0; i < LANG_CHAR_COUNT_EN; i++)
    {
      lang_weight_EN_cs[i] = i;
      lang_next_alpha_char_EN_cs[i] = i + 1;
    }

  lang_weight_EN_cs[32] = 0;
  lang_next_alpha_char_EN_cs[32] = 1;

  is_common_en_cs_init = true;
}

/*
 * lang_init_common_en_ci () - init collation data for English case
 *			       insensitive (no matter the charset)
 *   return:
 */
static void
lang_init_common_en_ci (void)
{
  int i;
  static bool is_common_en_ci_init = false;

  if (is_common_en_ci_init)
    {
      return;
    }

  for (i = 0; i < LANG_CHAR_COUNT_EN; i++)
    {
      lang_weight_EN_ci[i] = i;
      lang_next_alpha_char_EN_ci[i] = i + 1;
    }

  for (i = 'a'; i <= (int) 'z'; i++)
    {
      lang_weight_EN_ci[i] = i - ('a' - 'A');
      lang_next_alpha_char_EN_ci[i] = i + 1 - ('a' - 'A');
    }

  lang_next_alpha_char_EN_ci['z'] = lang_next_alpha_char_EN_ci['Z'];
  lang_next_alpha_char_EN_ci['a' - 1] = lang_next_alpha_char_EN_ci['A' - 1];

  lang_weight_EN_ci[32] = 0;
  lang_next_alpha_char_EN_ci[32] = 1;

  is_common_en_ci_init = true;
}

/*
 * lang_init_coll_utf8_en_cs () - init collation UTF8 English case sensitive
 *   return:
 */
static void
lang_init_coll_utf8_en_cs (LANG_COLLATION * lang_coll)
{
  assert (lang_coll != NULL);

  if (!(lang_coll->need_init))
    {
      return;
    }

  /* init data */
  lang_init_common_en_cs ();

  lang_coll->need_init = false;
}

/*
 * lang_init_coll_en_ci () - init collation English case insensitive; applies
 *			     to both ISO and UTF-8 charset
 *   return:
 */
static void
lang_init_coll_en_ci (LANG_COLLATION * lang_coll)
{
  assert (lang_coll != NULL);

  if (!(lang_coll->need_init))
    {
      return;
    }

  /* init data */
  lang_init_common_en_ci ();

  lang_coll->need_init = false;
}

/*
 * lang_initloc_en () - init locale data for English language
 *   return:
 */
static void
lang_initloc_en_utf8 (LANG_LOCALE_DATA * ld)
{
  int i;

  assert (ld != NULL);

  assert (ld->default_lang_coll != NULL);

  /* init alphabet */
  for (i = 0; i < LANG_CHAR_COUNT_EN; i++)
    {
      lang_upper_EN[i] = i;
      lang_lower_EN[i] = i;
    }

  for (i = (int) 'a'; i <= (int) 'z'; i++)
    {
      lang_upper_EN[i] = i - ('a' - 'A');
      lang_lower_EN[i - ('a' - 'A')] = i;
    }

  /* other initializations to follow here */
  coll_utf8_binary.default_lang = ld;
  coll_utf8_en_cs.default_lang = ld;
  coll_utf8_en_ci.default_lang = ld;

  ld->is_initialized = true;
}

/*
 * lang_fastcmp_iso_88591 () - compare two character strings of ISO-8859-1
 *			       codeset
 *
 * Arguments:
 *    lang_coll: collation data
 *      string1: 1st character string
 *        size1: size of 1st string
 *      string2: 2nd character string
 *        size2: size of 2nd string
 *
 * Returns:
 *   Greater than 0 if string1 > string2
 *   Equal to 0     if string1 = string2
 *   Less than 0    if string1 < string2
 *
 * Errors:
 *
 * Note:
 *   This function is similar to strcmp(3) or bcmp(3). It is designed to
 *   follow SQL_TEXT character set collation. Padding character(space ' ') is
 *   the smallest character in the set. (e.g.) "ab z" < "ab\t1"
 *
 */

static int
lang_fastcmp_iso_88591 (const LANG_COLLATION * lang_coll,
			const unsigned char *string1, const int size1,
			const unsigned char *string2, const int size2)
{
  int n, i, cmp;
  unsigned char c1, c2;

#define PAD ' '			/* str_pad_char(INTL_CODESET_ISO88591, pad, &pad_size) */
#define SPACE PAD		/* smallest character in the collation sequence */
#define ZERO '\0'		/* space is treated as zero */

  n = size1 < size2 ? size1 : size2;
  for (i = 0, cmp = 0; i < n && cmp == 0; i++)
    {
      c1 = *string1++;
      if (c1 == SPACE)
	{
	  c1 = ZERO;
	}
      c2 = *string2++;
      if (c2 == SPACE)
	{
	  c2 = ZERO;
	}
      cmp = c1 - c2;
    }
  if (cmp != 0)
    {
      return cmp;
    }
  if (size1 == size2)
    {
      return cmp;
    }

  c1 = c2 = ZERO;
  if (size1 < size2)
    {
      n = size2 - size1;
      for (i = 0; i < n && cmp == 0; i++)
	{
	  c2 = *string2++;
	  if (c2 == PAD)
	    {
	      c2 = ZERO;
	    }
	  cmp = c1 - c2;
	}
    }
  else
    {
      n = size1 - size2;
      for (i = 0; i < n && cmp == 0; i++)
	{
	  c1 = *string1++;
	  if (c1 == PAD)
	    {
	      c1 = ZERO;
	    }
	  cmp = c1 - c2;
	}
    }
  return cmp;

#undef PAD
#undef SPACE
#undef ZERO
}

/*
 * lang_strmatch_iso_88591 () - match or compare two character strings of
 *			        ISO-8859-1 codeset
 *
 *   return: negative if str1 < str2, positive if str1 > str2, zero otherwise
 *   lang_coll(in) : collation data
 *   is_match(in) : true if match, otherwise is compare
 *   str1(in):
 *   size1(in):
 *   str2(in): this is the pattern string in case of match
 *   size2(in):
 *   escape(in): pointer to escape character (multi-byte allowed)
 *		 (used in context of LIKE)
 *   has_last_escape(in): true if it should check if last character is the
 *			  escape character
 *   str1_match_size(out): size from str1 which is matched with str2
 */
static int
lang_strmatch_iso_88591 (const LANG_COLLATION * lang_coll, bool is_match,
			 const unsigned char *str1, int size1,
			 const unsigned char *str2, int size2,
			 const unsigned char *escape,
			 const bool has_last_escape, int *str1_match_size)
{
  unsigned int c1, c2;
  const unsigned char *str1_end;
  const unsigned char *str2_end;
  const unsigned char *str1_begin;
  const int alpha_cnt = lang_coll->coll.w_count;
  const unsigned int *weight_ptr = lang_coll->coll.weights;

#define PAD ' '			/* str_pad_char(INTL_CODESET_ISO88591, pad, &pad_size) */
#define SPACE PAD		/* smallest character in the collation sequence */
#define ZERO '\0'		/* space is treated as zero */

  str1_begin = str1;
  str1_end = str1 + size1;
  str2_end = str2 + size2;
  for (; str1 < str1_end && str2 < str2_end;)
    {
      assert (str1_end - str1 > 0);
      assert (str2_end - str2 > 0);

      c1 = *str1++;
      if (c1 == SPACE)
	{
	  c1 = ZERO;
	}
      c2 = *str2++;
      if (c2 == SPACE)
	{
	  c2 = ZERO;
	}

      if (is_match && escape != NULL && c2 == *escape)
	{
	  if (!(has_last_escape && str2 + 1 >= str2_end))
	    {
	      c2 = *str2++;
	      if (c2 == SPACE)
		{
		  c2 = ZERO;
		}
	    }
	}

      if (c1 < (unsigned int) alpha_cnt)
	{
	  c1 = weight_ptr[c1];
	}
      if (c2 < (unsigned int) alpha_cnt)
	{
	  c2 = weight_ptr[c2];
	}

      if (c1 != c2)
	{
	  return (c1 < c2) ? -1 : 1;
	}
    }

  size1 = str1_end - str1;
  size2 = str2_end - str2;

  assert (size1 == 0 || size2 == 0);

  if (is_match)
    {
      assert (str1_match_size != NULL);
      *str1_match_size = str1 - str1_begin;
    }

  if (size1 == size2)
    {
      return 0;
    }
  else if (size2 > 0)
    {
      if (is_match)
	{
	  /* pattern string should be exhausted for a full match */
	  return -1;
	}
      for (; str2 < str2_end;)
	{
	  c2 = *str2++;
	  if (c2 != PAD && c2 != 0)
	    {
	      return -1;
	    }
	}
    }
  else
    {
      assert (size1 > 0);

      if (is_match)
	{
	  return 0;
	}

      for (; str1 < str1_end;)
	{
	  c1 = *str1++;
	  if (c1 != PAD && c1 != ZERO)
	    {
	      return 1;
	    }
	}
    }
  return 0;

#undef PAD
#undef SPACE
#undef ZERO
}

/*
 * lang_fastcmp_byte () - string compare for English language in UTF-8
 *   return:
 *   lang_coll(in):
 *   string1(in):
 *   size1(in):
 *   string2(in):
 *   size2(in):
 *
 * Note: This string comparison ignores trailing white spaces.
 */
static int
lang_fastcmp_byte (const LANG_COLLATION * lang_coll,
		   const unsigned char *string1, const int size1,
		   const unsigned char *string2, const int size2)
{
  int cmp, i, size;

  size = size1 < size2 ? size1 : size2;
  for (cmp = 0, i = 0; cmp == 0 && i < size; i++)
    {
      /* compare weights of the two chars */
      cmp = lang_coll->coll.weights[*string1++] -
	lang_coll->coll.weights[*string2++];
    }
  if (cmp != 0 || size1 == size2)
    {
      return cmp;
    }

  if (size1 < size2)
    {
      size = size2 - size1;
      for (i = 0; i < size && cmp == 0; i++)
	{
	  /* ignore tailing white spaces */
	  if (lang_coll->coll.weights[*string2++])
	    {
	      return -1;
	    }
	}
    }
  else
    {
      size = size1 - size2;
      for (i = 0; i < size && cmp == 0; i++)
	{
	  /* ignore trailing white spaces */
	  if (lang_coll->coll.weights[*string1++])
	    {
	      return 1;
	    }
	}
    }

  return cmp;
}

/*
 * lang_next_alpha_char_iso88591() - computes the next alphabetical char
 *   return: size in bytes of the next alphabetical char
 *   lang_coll(in): collation data
 *   seq(in): pointer to current char
 *   size(in): size in bytes for seq
 *   next_seq(in/out): buffer to return next alphabetical char
 *   len_next(in/out): length in chars for nex_seq
 *
 */
static int
lang_next_alpha_char_iso88591 (const LANG_COLLATION * lang_coll,
			       const unsigned char *seq, const int size,
			       unsigned char *next_seq, int *len_next)
{
  assert (seq != NULL);
  assert (next_seq != NULL);
  assert (len_next != NULL);
  assert (size > 0);

  *next_seq = (*seq == 0xff) ? 0xff : (*seq + 1);
  *len_next = 1;
  return 1;
}

/*
 * lang_next_coll_byte() - computes the next collatable char
 *   return: size in bytes of the next collatable char
 *   lang_coll(on): collation
 *   seq(in): pointer to current char
 *   size(in): available bytes for current char
 *   next_seq(in/out): buffer to return next alphabetical char
 *   len_next(in/out): length in chars of next char (always 1 for this func)
 *
 *  Note :  This assumes the weights and next col are define at byte level.
 */
static int
lang_next_coll_byte (const LANG_COLLATION * lang_coll,
		     const unsigned char *seq, const int size,
		     unsigned char *next_seq, int *len_next)
{
  unsigned int cp_alpha_char, cp_next_alpha_char;
  const int alpha_cnt = lang_coll->coll.w_count;
  const unsigned int *next_alpha_char = lang_coll->coll.next_cp;
  unsigned char *dummy = NULL;

  assert (seq != NULL);
  assert (next_seq != NULL);
  assert (len_next != NULL);
  assert (size > 0);

  cp_alpha_char = (unsigned int) *seq;

  if (cp_alpha_char < (unsigned int) alpha_cnt)
    {
      cp_next_alpha_char = next_alpha_char[cp_alpha_char];
    }
  else
    {
      cp_next_alpha_char =
	(cp_alpha_char == 0xff) ? 0xff : (cp_alpha_char + 1);
    }

  assert (cp_next_alpha_char <= 0xff);

  *next_seq = (unsigned char) cp_next_alpha_char;
  *len_next = 1;

  return 1;
}


static LANG_LOCALE_DATA lc_English_iso88591 = {
  NULL,
  LANG_NAME_ENGLISH,
  INTL_LANG_ENGLISH,
  INTL_CODESET_ISO88591,
  /* alphabet for user strings */
  {ALPHABET_TAILORED, INTL_CODESET_ISO88591, 0, 0, NULL, 0, NULL, false},
  /* alphabet for identifiers strings */
  {ALPHABET_TAILORED, INTL_CODESET_ISO88591, 0, 0, NULL, 0, NULL, false},
  &coll_iso88591_en_cs,
  NULL,				/* console text conversion */
  false,
  NULL,				/* time, date, date-time, timestamp format */
  NULL,
  NULL,
  NULL,
  {NULL},
  {NULL},
  {NULL},
  {NULL},
  {NULL},
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  '.',
  ',',
  DB_CURRENCY_DOLLAR,
  LANG_NO_NORMALIZATION,
  "6ae1bf7f15e6f132c4361761d203c1b4",
  lang_initloc_en_iso88591,
  false
};

static LANG_LOCALE_DATA lc_English_utf8 = {
  NULL,
  LANG_NAME_ENGLISH,
  INTL_LANG_ENGLISH,
  INTL_CODESET_UTF8,
  {ALPHABET_ASCII, INTL_CODESET_UTF8, LANG_CHAR_COUNT_EN, 1, lang_lower_EN, 1,
   lang_upper_EN,
   false},
  {ALPHABET_ASCII, INTL_CODESET_UTF8, LANG_CHAR_COUNT_EN, 1, lang_lower_EN, 1,
   lang_upper_EN,
   false},
  &coll_utf8_en_cs,
  &con_iso_8859_1_conv,		/* text conversion */
  false,
  NULL,				/* time, date, date-time, timestamp format */
  NULL,
  NULL,
  NULL,
  {NULL},
  {NULL},
  {NULL},
  {NULL},
  {NULL},
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  '.',
  ',',
  DB_CURRENCY_DOLLAR,
  LANG_NO_NORMALIZATION,
  "945bead220ece6f4d020403835308785",
  lang_initloc_en_utf8,
  false
};


/*
 * Turkish Locale Data
 */

/* Turkish collation */
static unsigned int lang_upper_TR[LANG_CHAR_COUNT_TR];
static unsigned int lang_lower_TR[LANG_CHAR_COUNT_TR];
static unsigned int lang_upper_i_TR[LANG_CHAR_COUNT_TR];
static unsigned int lang_lower_i_TR[LANG_CHAR_COUNT_TR];

static char lang_time_format_TR[] = "HH24:MI:SS";
static char lang_date_format_TR[] = "DD.MM.YYYY";
static char lang_datetime_format_TR[] = "HH24:MI:SS.FF DD.MM.YYYY";
static char lang_timestamp_format_TR[] = "HH24:MI:SS DD.MM.YYYY";

/*
 * lang_init_coll_utf8_tr_cs () - init collation data for Turkish
 *   return:
 *   lang_coll(in):
 */
static void
lang_init_coll_utf8_tr_cs (LANG_COLLATION * lang_coll)
{
  int i;

  const unsigned int special_upper_cp[] = {
    0xc7,			/* capital C with cedilla */
    0x11e,			/* capital letter G with breve */
    0x130,			/* capital letter I with dot above */
    0xd6,			/* capital letter O with diaeresis */
    0x15e,			/* capital letter S with cedilla */
    0xdc			/* capital letter U with diaeresis */
  };

  const unsigned int special_prev_upper_cp[] =
    { 'C', 'G', 'I', 'O', 'S', 'U' };

  const unsigned int special_lower_cp[] = {
    0xe7,			/* small c with cedilla */
    0x11f,			/* small letter g with breve */
    0x131,			/* small letter dotless i */
    0xf6,			/* small letter o with diaeresis */
    0x15f,			/* small letter s with cedilla */
    0xfc			/* small letter u with diaeresis */
  };

  const unsigned int special_prev_lower_cp[] =
    { 'c', 'g', 'h', 'o', 's', 'u' };

  assert (lang_coll != NULL);

  if (!(lang_coll->need_init))
    {
      return;
    }

  for (i = 0; i < LANG_CHAR_COUNT_TR; i++)
    {
      lang_weight_TR[i] = i;

      lang_next_alpha_char_TR[i] = i + 1;
    }

  lang_weight_TR[32] = 0;
  lang_next_alpha_char_TR[32] = 1;

  assert (DIM (special_lower_cp) == DIM (special_upper_cp));

  /* specific turkish letters:  weighting for string compare */
  for (i = 0; i < (int) DIM (special_upper_cp); i++)
    {
      unsigned int j;
      unsigned int cp = special_upper_cp[i];
      unsigned cp_repl = 1 + special_prev_upper_cp[i];
      unsigned int w_repl = lang_weight_TR[cp_repl];

      lang_weight_TR[cp] = w_repl;

      assert (cp_repl < cp);
      for (j = cp_repl; j < cp; j++)
	{
	  if (lang_weight_TR[j] >= w_repl)
	    {
	      (lang_weight_TR[j])++;
	    }
	}
    }

  for (i = 0; i < (int) DIM (special_lower_cp); i++)
    {
      unsigned int j;
      unsigned int cp = special_lower_cp[i];
      unsigned cp_repl = 1 + special_prev_lower_cp[i];
      unsigned int w_repl = lang_weight_TR[cp_repl];

      lang_weight_TR[cp] = w_repl;

      assert (cp_repl < cp);
      for (j = cp_repl; j < cp; j++)
	{
	  if (lang_weight_TR[j] >= w_repl)
	    {
	      (lang_weight_TR[j])++;
	    }
	}
    }

  /* next letter in alphabet (for pattern searching) */
  for (i = 0; i < (int) DIM (special_upper_cp); i++)
    {
      unsigned int cp_special = special_upper_cp[i];
      unsigned int cp_prev = special_prev_upper_cp[i];
      unsigned int cp_next = cp_prev + 1;

      lang_next_alpha_char_TR[cp_prev] = cp_special;
      lang_next_alpha_char_TR[cp_special] = cp_next;
    }

  for (i = 0; i < (int) DIM (special_lower_cp); i++)
    {
      unsigned int cp_special = special_lower_cp[i];
      unsigned int cp_prev = special_prev_lower_cp[i];
      unsigned int cp_next = cp_prev + 1;

      lang_next_alpha_char_TR[cp_prev] = cp_special;
      lang_next_alpha_char_TR[cp_special] = cp_next;
    }

  /* other initializations to follow here */

  lang_coll->need_init = false;
}

/*
 * lang_initloc_tr_iso () - init locale data for Turkish language
 *			    (ISO charset)
 *   return:
 *   ld(in/out):
 */
static void
lang_initloc_tr_iso (LANG_LOCALE_DATA * ld)
{
  assert (ld != NULL);

  ld->is_initialized = true;
}

/*
 * lang_initloc_tr_utf8 () - init locale data for Turkish language (UTF8)
 *   return:
 *   ld(in/out):
 */
static void
lang_initloc_tr_utf8 (LANG_LOCALE_DATA * ld)
{
  int i;

  const unsigned int special_upper_cp[] = {
    0xc7,			/* capital C with cedilla */
    0x11e,			/* capital letter G with breve */
    0x130,			/* capital letter I with dot above */
    0xd6,			/* capital letter O with diaeresis */
    0x15e,			/* capital letter S with cedilla */
    0xdc			/* capital letter U with diaeresis */
  };

  const unsigned int special_prev_upper_cp[] =
    { 'C', 'G', 'I', 'O', 'S', 'U' };

  const unsigned int special_lower_cp[] = {
    0xe7,			/* small c with cedilla */
    0x11f,			/* small letter g with breve */
    0x131,			/* small letter dotless i */
    0xf6,			/* small letter o with diaeresis */
    0x15f,			/* small letter s with cedilla */
    0xfc			/* small letter u with diaeresis */
  };

  const unsigned int special_prev_lower_cp[] =
    { 'c', 'g', 'h', 'o', 's', 'u' };

  assert (ld != NULL);

  assert (ld->default_lang_coll != NULL);

  /* init alphabet */
  for (i = 0; i < LANG_CHAR_COUNT_TR; i++)
    {
      lang_upper_TR[i] = i;
      lang_lower_TR[i] = i;
    }

  for (i = (int) 'a'; i <= (int) 'z'; i++)
    {
      lang_upper_TR[i] = i - ('a' - 'A');
      lang_lower_TR[i - ('a' - 'A')] = i;

      lang_lower_TR[i] = i;
      lang_upper_TR[i - ('a' - 'A')] = i - ('a' - 'A');
    }

  assert (DIM (special_lower_cp) == DIM (special_upper_cp));
  /* specific turkish letters: */
  for (i = 0; i < (int) DIM (special_lower_cp); i++)
    {
      lang_lower_TR[special_lower_cp[i]] = special_lower_cp[i];
      lang_upper_TR[special_lower_cp[i]] = special_upper_cp[i];

      lang_lower_TR[special_upper_cp[i]] = special_lower_cp[i];
      lang_upper_TR[special_upper_cp[i]] = special_upper_cp[i];
    }

  memcpy (lang_upper_i_TR, lang_upper_TR,
	  LANG_CHAR_COUNT_TR * sizeof (lang_upper_TR[0]));
  memcpy (lang_lower_i_TR, lang_lower_TR,
	  LANG_CHAR_COUNT_TR * sizeof (lang_lower_TR[0]));

  /* identifiers alphabet : same as Unicode data */
  lang_upper_i_TR[0x131] = 'I';	/* small letter dotless i */
  lang_lower_i_TR[0x130] = 'i';	/* capital letter I with dot above */

  /* exceptions in TR casing for user alphabet :
   */
  lang_upper_TR[0x131] = 'I';	/* small letter dotless i */
  lang_lower_TR[0x131] = 0x131;	/* small letter dotless i */
  lang_upper_TR['i'] = 0x130;	/* capital letter I with dot above */
  lang_lower_TR['i'] = 'i';

  lang_lower_TR[0x130] = 'i';	/* capital letter I with dot above */
  lang_upper_TR[0x130] = 0x130;	/* capital letter I with dot above */
  lang_upper_TR['I'] = 'I';
  lang_lower_TR['I'] = 0x131;	/* small letter dotless i */

  /* other initializations to follow here */
  coll_utf8_tr_cs.default_lang = ld;

  ld->is_initialized = true;
}

/* Turkish in ISO-8859-1 charset : limited support (only date - formats) */
static LANG_LOCALE_DATA lc_Turkish_iso88591 = {
  NULL,
  LANG_NAME_TURKISH,
  INTL_LANG_TURKISH,
  INTL_CODESET_ISO88591,
  /* user alphabet : same as English ISO */
  {ALPHABET_TAILORED, INTL_CODESET_ISO88591, 0, 0, NULL, 0, NULL, false},
  /* identifiers alphabet : same as English ISO */
  {ALPHABET_TAILORED, INTL_CODESET_ISO88591, 0, 0, NULL, 0, NULL, false},
  &coll_iso88591_en_cs,		/* collation : same as English ISO */
  NULL,				/* console text conversion */
  false,
  lang_time_format_TR,
  lang_date_format_TR,
  lang_datetime_format_TR,
  lang_timestamp_format_TR,
  {NULL},
  {NULL},
  {NULL},
  {NULL},
  {NULL},
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  ',',
  '.',
  DB_CURRENCY_TL,
  LANG_NO_NORMALIZATION,
  "b9ac135bdf8100b205ebb6b7e0e9c3df",
  lang_initloc_tr_iso,
  false
};

static LANG_LOCALE_DATA lc_Turkish_utf8 = {
  NULL,
  LANG_NAME_TURKISH,
  INTL_LANG_TURKISH,
  INTL_CODESET_UTF8,
  {ALPHABET_ASCII, INTL_CODESET_UTF8, LANG_CHAR_COUNT_TR, 1, lang_lower_TR, 1,
   lang_upper_TR, false},
  {ALPHABET_TAILORED, INTL_CODESET_UTF8, LANG_CHAR_COUNT_TR, 1,
   lang_lower_i_TR, 1, lang_upper_i_TR, false},
  &coll_utf8_tr_cs,
  &con_iso_8859_9_conv,		/* console text conversion */
  false,
  lang_time_format_TR,
  lang_date_format_TR,
  lang_datetime_format_TR,
  lang_timestamp_format_TR,
  {NULL},
  {NULL},
  {NULL},
  {NULL},
  {NULL},
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  ',',
  '.',
  DB_CURRENCY_TL,
  LANG_NO_NORMALIZATION,
  "a6c90a844ad44f78d0b1a3a9a87ddb2f",
  lang_initloc_tr_utf8,
  false
};

/*
 * Korean Locale Data
 */

/*
 * lang_initloc_ko_iso () - init locale data for Korean language with ISO
 *			    charset
 *   return:
 */
static void
lang_initloc_ko_iso (LANG_LOCALE_DATA * ld)
{
  assert (ld != NULL);

  ld->is_initialized = true;
}

/*
 * lang_initloc_ko_utf8 () - init locale data for Korean language with UTF-8
 *			     charset
 *   return:
 */
static void
lang_initloc_ko_utf8 (LANG_LOCALE_DATA * ld)
{
  assert (ld != NULL);

  coll_utf8_ko_cs.default_lang = ld;

  ld->is_initialized = true;
}


/*
 * lang_initloc_ko_euc () - init locale data for Korean language with EUC-KR
 *			    charset
 *   return:
 */
static void
lang_initloc_ko_euc (LANG_LOCALE_DATA * ld)
{
  assert (ld != NULL);

  coll_euckr_bin.default_lang = ld;

  ld->is_initialized = true;
}

/*
 * lang_fastcmp_ko () - compare two EUC-KR character strings
 *
 * Arguments:
 *    lang_coll: collation data
 *      string1: 1st character string
 *        size1: size of 1st string
 *      string2: 2nd character string
 *        size2: size of 2nd string
 *
 * Returns:
 *   Greater than 0 if string1 > string2
 *   Equal to 0     if string1 = string2
 *   Less than 0    if string1 < string2
 *
 */
static int
lang_fastcmp_ko (const LANG_COLLATION * lang_coll,
		 const unsigned char *string1, const int size1,
		 const unsigned char *string2, const int size2)
{
  int n, i, cmp, pad_size = 0;
  unsigned char c1, c2, pad[2];

  assert (size1 >= 0 && size2 >= 0);

  pad[0] = pad[1] = '\241';
  pad_size = 2;

#define PAD pad[i % pad_size]
#define SPACE PAD		/* smallest character in the collation sequence */
#define ZERO '\0'		/* space is treated as zero */
  n = size1 < size2 ? size1 : size2;
  for (i = 0, cmp = 0; i < n && cmp == 0; i++)
    {
      c1 = *string1++;
      if (c1 == SPACE)
	{
	  c1 = ZERO;
	}
      c2 = *string2++;
      if (c2 == SPACE)
	{
	  c2 = ZERO;
	}
      cmp = c1 - c2;
    }
  if (cmp != 0)
    {
      return cmp;
    }
  if (size1 == size2)
    {
      return cmp;
    }

  c1 = c2 = ZERO;
  if (size1 < size2)
    {
      n = size2 - size1;
      for (i = 0; i < n && cmp == 0; i++)
	{
	  c2 = *string2++;
	  if (c2 == PAD)
	    {
	      c2 = ZERO;
	    }
	  cmp = c1 - c2;
	}
    }
  else
    {
      n = size1 - size2;
      for (i = 0; i < n && cmp == 0; i++)
	{
	  c1 = *string1++;
	  if (c1 == PAD)
	    {
	      c1 = ZERO;
	    }
	  cmp = c1 - c2;
	}
    }
  return cmp;
#undef SPACE
#undef ZERO
#undef PAD
}

/*
 * lang_strmatch_ko () - compare two EUC-KR character strings
 *
 *   return: negative if str1 < str2, positive if str1 > str2, zero otherwise
 *   lang_coll(in) : collation data
 *   is_match(in) : true if match, otherwise is compare
 *   str1(in):
 *   size1(in):
 *   str2(in): this is the pattern string in case of match
 *   size2(in):
 *   escape(in): pointer to escape character (multi-byte allowed)
 *		 (used in context of LIKE)
 *   has_last_escape(in): true if it should check if last character is the
 *			  escape character
 *   str1_match_size(out): size from str1 which is matched with str2
 *
 */
static int
lang_strmatch_ko (const LANG_COLLATION * lang_coll, bool is_match,
		  const unsigned char *str1, int size1,
		  const unsigned char *str2, int size2,
		  const unsigned char *escape, const bool has_last_escape,
		  int *str1_match_size)
{
  const unsigned char *str1_end;
  const unsigned char *str2_end;
  const unsigned char *str1_next;
  const unsigned char *str2_next;
  const unsigned char *str1_begin;
  const unsigned char *str2_begin;
  int char1_size, char2_size, cmp = 0, i, pad_size = 0;
  unsigned char c1, c2, pad[2];

  assert (size1 >= 0 && size2 >= 0);

  pad[0] = pad[1] = '\241';
  pad_size = 2;

#define PAD pad[i % pad_size]
#define ZERO '\0'		/* space is treated as zero */

  str1_begin = str1;
  str2_begin = str2;
  str1_end = str1 + size1;
  str2_end = str2 + size2;

  for (; str1 < str1_end && str2 < str2_end;)
    {
      assert (str1_end - str1 > 0);
      assert (str2_end - str2 > 0);

      str1_next = intl_nextchar_euc ((unsigned char *) str1, &char1_size);
      str2_next = intl_nextchar_euc ((unsigned char *) str2, &char2_size);

      if (is_match && escape != NULL
	  && memcmp (str2, escape, char2_size) == 0)
	{
	  if (!(has_last_escape && str2_next >= str2_end))
	    {
	      str2 = str2_next;
	      str2_next = intl_nextchar_euc ((unsigned char *) str2,
					     &char2_size);
	    }
	}

      if (char1_size != char2_size)
	{
	  return (char1_size < char2_size) ? (-1) : 1;
	}
      else
	{
	  cmp = memcmp (str1, str2, char1_size);
	  if (cmp != 0)
	    {
	      return cmp;
	    }
	}

      str1 = str1_next;
      str2 = str2_next;
    }

  size1 = str1_end - str1;
  size2 = str2_end - str2;

  if (is_match)
    {
      assert (str1_match_size != NULL);
      *str1_match_size = str1 - str1_begin;
    }

  assert (size1 == 0 || size2 == 0);
  assert (cmp == 0);

  c1 = c2 = ZERO;
  if (size1 == size2)
    {
      return 0;
    }
  else if (size2 > 0)
    {
      if (is_match)
	{
	  return -1;
	}
      for (i = 0; i < size2; i++)
	{
	  c2 = *str2++;
	  if (c2 != PAD && c2 != ZERO)
	    {
	      return -1;
	    }
	}
    }
  else
    {
      assert (size1 > 0);

      if (is_match)
	{
	  return 0;
	}

      for (i = 0; i < size1; i++)
	{
	  c1 = *str1++;
	  if (c1 != PAD && c1 != ZERO)
	    {
	      return 1;
	    }
	}
    }
  return cmp;
#undef ZERO
#undef PAD
}

/*
 * lang_next_alpha_char_ko() - computes the next alphabetical char
 *   return: size in bytes of the next alphabetical char
 *   lang_coll(in): collation data
 *   seq(in): pointer to current char
 *   size(in): size in bytes for seq
 *   next_seq(in/out): buffer to return next alphabetical char
 *   len_next(in/out): length in chars for nex_seq
 */
static int
lang_next_alpha_char_ko (const LANG_COLLATION * lang_coll,
			 const unsigned char *seq, const int size,
			 unsigned char *next_seq, int *len_next)
{
  int char_size;
  assert (seq != NULL);
  assert (next_seq != NULL);
  assert (len_next != NULL);
  assert (size > 0);

  (void) intl_char_size ((unsigned char *) seq, 1, INTL_CODESET_KSC5601_EUC,
			 &char_size);
  memcpy (next_seq, seq, char_size);

  assert (char_size <= 3);
  /* increment last byte of current character without carry and without
   * mixing ASCII range with korean range;
   * this works for EUC-KR characters encoding which don't have terminal
   * byte = FF */
  if ((char_size == 1 && *next_seq < 0x7f)
      || (char_size > 1 && next_seq[char_size - 1] < 0xff))
    {
      next_seq[char_size - 1]++;
    }

  *len_next = 1;
  return char_size;
}


static LANG_LOCALE_DATA lc_Korean_iso88591 = {
  NULL,
  LANG_NAME_KOREAN,
  INTL_LANG_KOREAN,
  INTL_CODESET_ISO88591,
  /* alphabet : same as English ISO */
  {ALPHABET_TAILORED, INTL_CODESET_ISO88591, 0, 0, NULL, 0, NULL, false},
  /* identifiers alphabet : same as English ISO */
  {ALPHABET_TAILORED, INTL_CODESET_ISO88591, 0, 0, NULL, 0, NULL, false},
  &coll_iso88591_en_cs,		/* collation : same as English ISO */
  NULL,				/* console text conversion */
  false,
  NULL,				/* time, date, date-time, timestamp format */
  NULL,
  NULL,
  NULL,
  {NULL},
  {NULL},
  {NULL},
  {NULL},
  {NULL},
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  '.',
  ',',
  DB_CURRENCY_WON,
  LANG_NO_NORMALIZATION,
  "8710ffb79b191c2158d4c498e8bc7dea",
  lang_initloc_ko_iso,
  false
};

/* built-in support of Korean in UTF-8 : date-time conversions as in English
 * collation : by codepoints
 * this needs to be overriden by user defined locale */
static LANG_LOCALE_DATA lc_Korean_utf8 = {
  NULL,
  LANG_NAME_KOREAN,
  INTL_LANG_KOREAN,
  INTL_CODESET_UTF8,
  {ALPHABET_ASCII, INTL_CODESET_UTF8, LANG_CHAR_COUNT_EN, 1, lang_lower_EN, 1,
   lang_upper_EN, false},
  {ALPHABET_ASCII, INTL_CODESET_UTF8, LANG_CHAR_COUNT_EN, 1, lang_lower_EN, 1,
   lang_upper_EN, false},
  &coll_utf8_ko_cs,		/* collation */
  NULL,				/* console text conversion */
  false,
  NULL,				/* time, date, date-time, timestamp format */
  NULL,
  NULL,
  NULL,
  {NULL},
  {NULL},
  {NULL},
  {NULL},
  {NULL},
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  '.',
  ',',
  DB_CURRENCY_WON,
  LANG_NO_NORMALIZATION,
  "802cff8e10d857952241d19b50a13a27",
  lang_initloc_ko_utf8,
  false
};

/* built-in support of Korean in EUC-KR : date-time conversions as in English
 * collation : binary */
static LANG_LOCALE_DATA lc_Korean_euckr = {
  NULL,
  LANG_NAME_KOREAN,
  INTL_LANG_KOREAN,
  INTL_CODESET_KSC5601_EUC,
  /* alphabet */
  {ALPHABET_TAILORED, INTL_CODESET_KSC5601_EUC, 0, 0, NULL, 0, NULL, false},
  /* identifiers alphabet */
  {ALPHABET_TAILORED, INTL_CODESET_KSC5601_EUC, 0, 0, NULL, 0, NULL, false},
  &coll_euckr_bin,		/* collation */
  NULL,				/* console text conversion */
  false,
  NULL,				/* time, date, date-time, timestamp format */
  NULL,
  NULL,
  NULL,
  {NULL},
  {NULL},
  {NULL},
  {NULL},
  {NULL},
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  '.',
  ',',
  DB_CURRENCY_WON,
  LANG_NO_NORMALIZATION,
  "c46ff948b4147323edfba0c51f96fe47",
  lang_initloc_ko_euc,
  false
};

#if defined(WINDOWS)
#define GET_SYM_ADDR(lib, sym) GetProcAddress(lib, sym)
#else
#define GET_SYM_ADDR(lib, sym) dlsym(lib, sym)
#endif

#define SHLIB_GET_ADDR(v, SYM_NAME, SYM_TYPE, lh, LOC_NAME)               \
  do {				                                          \
    snprintf (sym_name, SYMBOL_NAME_SIZE, "" SYM_NAME "_%s", LOC_NAME);   \
    v = (SYM_TYPE) GET_SYM_ADDR (lh, sym_name);				  \
    if (v == NULL)			                                  \
      {					                                  \
	goto error_loading_symbol;	                                  \
      }					                                  \
  } while (0);				                                  \


#define SHLIB_GET_VAL(v, SYM_NAME, SYM_TYPE, lh, LOC_NAME)	\
  do {								\
    SYM_TYPE* aux;						\
    SHLIB_GET_ADDR(aux, SYM_NAME, SYM_TYPE*, lh, LOC_NAME)	\
    v = *aux;							\
  } while (0);


/*
 * lang_locale_data_load_from_lib() - loads locale data from shared libray
 *
 * return: error code
 * lld(out): lang locale data
 * lib_handle(in)
 * lf(in): locale file info
 * is_load_for_dump (in): true if load is in context of dump tool
 */
int
lang_locale_data_load_from_lib (LANG_LOCALE_DATA * lld,
				void *lib_handle, const LOCALE_FILE * lf,
				bool is_load_for_dump)
{
  char sym_name[SYMBOL_NAME_SIZE + 1];
  char err_msg[ERR_MSG_SIZE + PATH_MAX];
  char **temp_array_sym;
  int *temp_num_sym;
  int err_status = NO_ERROR;
  int i, count_coll_to_load;
  const char *alpha_suffix = NULL;
  bool load_w_identifier_name;
  int txt_conv_type;
  bool sym_loc_name_found = false;

  assert (lld != NULL);
  assert (lib_handle != NULL);
  assert (lf != NULL);
  assert (lf->locale_name != NULL);

  SHLIB_GET_ADDR (lld->lang_name, "locale_name", char *, lib_handle,
		  lf->locale_name);
  sym_loc_name_found = true;

  SHLIB_GET_ADDR (lld->checksum, "locale_checksum", char *, lib_handle,
		  lf->locale_name);
  if (strlen (lld->checksum) != 32)
    {
      snprintf (err_msg, sizeof (err_msg) - 1, "invalid checksum in locale"
		" library %s", lf->lib_file);
      err_status = ER_LOC_INIT;
      LOG_LOCALE_ERROR (err_msg, err_status, true);
      goto exit;
    }

  SHLIB_GET_ADDR (lld->date_format, "date_format", char *,
		  lib_handle, lld->lang_name);
  SHLIB_GET_ADDR (lld->time_format, "time_format", char *,
		  lib_handle, lld->lang_name);
  SHLIB_GET_ADDR (lld->datetime_format, "datetime_format", char *,
		  lib_handle, lld->lang_name);
  SHLIB_GET_ADDR (lld->timestamp_format, "timestamp_format", char *,
		  lib_handle, lld->lang_name);

  SHLIB_GET_ADDR (temp_array_sym, "month_names_abbreviated", char **,
		  lib_handle, lld->lang_name);
  for (i = 0; i < CAL_MONTH_COUNT; i++)
    {
      lld->month_short_name[i] = temp_array_sym[i];
    }

  SHLIB_GET_ADDR (temp_array_sym, "month_names_wide", char **,
		  lib_handle, lld->lang_name);
  for (i = 0; i < CAL_MONTH_COUNT; i++)
    {
      lld->month_name[i] = temp_array_sym[i];
    }

  SHLIB_GET_ADDR (temp_array_sym, "day_names_abbreviated", char **,
		  lib_handle, lld->lang_name);
  for (i = 0; i < CAL_DAY_COUNT; i++)
    {
      lld->day_short_name[i] = temp_array_sym[i];
    }

  SHLIB_GET_ADDR (temp_array_sym, "day_names_wide", char **,
		  lib_handle, lld->lang_name);
  for (i = 0; i < CAL_DAY_COUNT; i++)
    {
      lld->day_name[i] = temp_array_sym[i];
    }

  SHLIB_GET_ADDR (temp_array_sym, "am_pm", char **, lib_handle,
		  lld->lang_name);
  for (i = 0; i < CAL_AM_PM_COUNT; i++)
    {
      lld->am_pm[i] = temp_array_sym[i];
    }

  SHLIB_GET_ADDR (lld->day_short_parse_order, "day_names_abbr_parse_order",
		  char *, lib_handle, lld->lang_name);

  SHLIB_GET_ADDR (lld->day_parse_order, "day_names_wide_parse_order",
		  char *, lib_handle, lld->lang_name);

  SHLIB_GET_ADDR (lld->month_short_parse_order,
		  "month_names_abbr_parse_order", char *,
		  lib_handle, lld->lang_name);

  SHLIB_GET_ADDR (lld->month_parse_order,
		  "month_names_wide_parse_order", char *,
		  lib_handle, lld->lang_name);

  SHLIB_GET_ADDR (lld->am_pm_parse_order, "am_pm_parse_order", char *,
		  lib_handle, lld->lang_name);

  SHLIB_GET_VAL (lld->number_decimal_sym, "number_decimal_sym", char,
		 lib_handle, lld->lang_name);

  SHLIB_GET_VAL (lld->number_group_sym, "number_group_sym", char,
		 lib_handle, lld->lang_name);

  SHLIB_GET_VAL (lld->default_currency_code,
		 "default_currency_code", int, lib_handle, lld->lang_name);

  /* alphabet */
  SHLIB_GET_ADDR (temp_num_sym, "alphabet_a_type", int *,
		  lib_handle, lld->lang_name);
  assert (*temp_num_sym >= ALPHABET_UNICODE &&
	  *temp_num_sym <= ALPHABET_TAILORED);
  lld->alphabet.a_type = (ALPHABET_TYPE) * temp_num_sym;

  if (lld->alphabet.a_type == ALPHABET_UNICODE)
    {
      alpha_suffix = "unicode";
    }
  else if (lld->alphabet.a_type == ALPHABET_ASCII)
    {
      alpha_suffix = "ascii";
    }
  else
    {
      alpha_suffix = lld->lang_name;
    }
  err_status =
    lang_locale_load_alpha_from_lib (&(lld->alphabet), false, alpha_suffix,
				     lib_handle, lf);
  if (err_status != NO_ERROR)
    {
      goto exit;
    }

  /* identifier alphabet */
  SHLIB_GET_ADDR (temp_num_sym, "ident_alphabet_a_type", int *,
		  lib_handle, lld->lang_name);
  assert (*temp_num_sym >= ALPHABET_UNICODE &&
	  *temp_num_sym <= ALPHABET_TAILORED);
  lld->ident_alphabet.a_type = (ALPHABET_TYPE) * temp_num_sym;

  load_w_identifier_name = false;
  if (lld->ident_alphabet.a_type == ALPHABET_UNICODE)
    {
      alpha_suffix = "unicode";
    }
  else if (lld->ident_alphabet.a_type == ALPHABET_ASCII)
    {
      alpha_suffix = "ascii";
    }
  else
    {
      alpha_suffix = lld->lang_name;
      load_w_identifier_name = true;
    }

  err_status =
    lang_locale_load_alpha_from_lib (&(lld->ident_alphabet),
				     load_w_identifier_name, alpha_suffix,
				     lib_handle, lf);
  if (err_status != NO_ERROR)
    {
      goto exit;
    }

  /* console conversion */
  SHLIB_GET_VAL (txt_conv_type, "tc_conv_type", int, lib_handle,
		 lld->lang_name);

  if (txt_conv_type == TEXT_CONV_ISO_88591_BUILTIN)
    {
      lld->txt_conv = &con_iso_8859_1_conv;
    }
  else if (txt_conv_type == TEXT_CONV_ISO_88599_BUILTIN)
    {
      lld->txt_conv = &con_iso_8859_9_conv;
    }
  else if (txt_conv_type == TEXT_CONV_NO_CONVERSION)
    {
      lld->txt_conv = NULL;
    }
  else
    {
      unsigned char *is_lead_byte;
      assert (txt_conv_type == TEXT_CONV_GENERIC_1BYTE
	      || txt_conv_type == TEXT_CONV_GENERIC_2BYTE);

      lld->txt_conv = (TEXT_CONVERSION *) malloc (sizeof (TEXT_CONVERSION));
      if (lld->txt_conv == NULL)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY,
		  1, sizeof (TEXT_CONVERSION));
	  err_status = ER_OUT_OF_VIRTUAL_MEMORY;
	  goto exit;
	}
      memset (lld->txt_conv, 0, sizeof (TEXT_CONVERSION));

      lld->txt_conv->conv_type = txt_conv_type;

      SHLIB_GET_ADDR (is_lead_byte,
		      "tc_is_lead_byte", unsigned char *,
		      lib_handle, lld->lang_name);
      memcpy (lld->txt_conv->byte_flag, is_lead_byte, 256);

      SHLIB_GET_VAL (lld->txt_conv->utf8_first_cp,
		     "tc_utf8_first_cp", unsigned int,
		     lib_handle, lld->lang_name);

      SHLIB_GET_VAL (lld->txt_conv->utf8_last_cp,
		     "tc_utf8_last_cp", unsigned int, lib_handle,
		     lld->lang_name);

      SHLIB_GET_VAL (lld->txt_conv->text_first_cp,
		     "tc_text_first_cp", unsigned int,
		     lib_handle, lld->lang_name);

      SHLIB_GET_VAL (lld->txt_conv->text_last_cp,
		     "tc_text_last_cp", unsigned int, lib_handle,
		     lld->lang_name);

      SHLIB_GET_ADDR (lld->txt_conv->win_codepages, "tc_win_codepages",
		      char *, lib_handle, lld->lang_name);

      SHLIB_GET_ADDR (lld->txt_conv->nl_lang_str, "tc_nl_lang_str", char *,
		      lib_handle, lld->lang_name);

      SHLIB_GET_ADDR (lld->txt_conv->utf8_to_text,
		      "tc_utf8_to_text", CONV_CP_TO_BYTES *,
		      lib_handle, lld->lang_name);

      SHLIB_GET_ADDR (lld->txt_conv->text_to_utf8,
		      "tc_text_to_utf8", CONV_CP_TO_BYTES *,
		      lib_handle, lld->lang_name);
    }

  err_status = lang_locale_load_normalization_from_lib (&(lld->unicode_norm),
							lib_handle, lf);
  if (err_status != NO_ERROR)
    {
      goto exit;
    }

  /* collation data */
  if (is_load_for_dump)
    {
      goto exit;
    }

  err_status =
    lang_load_count_coll_from_lib (&count_coll_to_load, lib_handle, lf);
  if (err_status != NO_ERROR)
    {
      goto exit;
    }

  for (i = 0; i < count_coll_to_load; i++)
    {
      /* get name of collation */
      char *collation_name = NULL;
      LANG_COLLATION *lang_coll = NULL;
      COLL_DATA *coll = NULL;

      err_status =
	lang_load_get_coll_name_from_lib (i, &collation_name, lib_handle, lf);
      if (err_status != NO_ERROR)
	{
	  goto exit;
	}

      if (lang_get_collation_by_name (collation_name) != NULL)
	{
	  /* collation already loaded */
	  continue;
	}

      lang_coll = (LANG_COLLATION *) malloc (sizeof (LANG_COLLATION));
      if (lang_coll == NULL)
	{
	  LOG_LOCALE_ERROR ("memory allocation failed", ER_LOC_INIT, true);
	  err_status = ER_LOC_INIT;
	  goto exit;
	}
      memset (lang_coll, 0, sizeof (LANG_COLLATION));

      assert (strlen (collation_name)
	      < (int) sizeof (lang_coll->coll.coll_name));
      strncpy (lang_coll->coll.coll_name, collation_name,
	       sizeof (lang_coll->coll.coll_name) - 1);

      coll = &(lang_coll->coll);
      err_status = lang_load_coll_from_lib (coll, lib_handle, lf);
      if (err_status != NO_ERROR)
	{
	  assert (lang_coll != NULL);
	  free (lang_coll);
	  goto exit;
	}

      lang_coll->codeset = INTL_CODESET_UTF8;
      lang_coll->built_in = 0;

      /* by default enable optimizations */
      lang_coll->options.allow_like_rewrite = true;
      lang_coll->options.allow_index_cov = true;
      lang_coll->options.allow_prefix_index = true;

      if (coll->uca_opt.sett_strength < TAILOR_QUATERNARY)
	{
	  lang_coll->options.allow_index_cov = false;
	  lang_coll->options.allow_like_rewrite = false;
	}

      if (coll->uca_exp_num > 1)
	{
	  lang_coll->fastcmp = lang_strcmp_utf8_uca;
	  lang_coll->strmatch = lang_strmatch_utf8_uca;
	  lang_coll->next_coll_seq = lang_next_coll_seq_utf8_w_contr;
	  lang_coll->split_key = lang_split_key_w_exp;
	  lang_coll->options.allow_like_rewrite = false;
	  lang_coll->options.allow_prefix_index = false;
	}
      else if (coll->count_contr > 0)
	{
	  lang_coll->fastcmp = lang_strcmp_utf8_w_contr;
	  lang_coll->strmatch = lang_strmatch_utf8_w_contr;
	  lang_coll->next_coll_seq = lang_next_coll_seq_utf8_w_contr;
	  lang_coll->split_key = lang_split_key_utf8;
	}
      else
	{
	  lang_coll->fastcmp = lang_strcmp_utf8;
	  lang_coll->strmatch = lang_strmatch_utf8;
	  lang_coll->next_coll_seq = lang_next_coll_char_utf8;
	  lang_coll->split_key = lang_split_key_utf8;
	}

      err_status = register_collation (lang_coll);
      if (err_status != NO_ERROR)
	{
	  assert (lang_coll != NULL);
	  free (lang_coll);
	  goto exit;
	}

      lang_coll->default_lang = lld;

      /* first collation in locale is the default collation of locale */
      if (lld->default_lang_coll == NULL)
	{
	  lld->default_lang_coll = lang_coll;
	}
    }


exit:
  return err_status;

error_loading_symbol:
  snprintf (err_msg, sizeof (err_msg) - 1,
	    "Cannot load symbol %s from the library file %s "
	    "for the %s locale!", sym_name, lf->lib_file, lf->locale_name);
  if (!sym_loc_name_found)
    {
      strcat (err_msg, "\n Locale might not be compiled into the selected "
	      "library.\n Check configuration and recompile locale"
	      ", if necessary,\n using the make_locale script");
    }
  LOG_LOCALE_ERROR (err_msg, ER_LOC_INIT, is_load_for_dump);

  return ER_LOC_INIT;
}

/*
 * lang_load_count_coll_from_lib() - reads and returns the number of
 *				     collations in library
 *
 * return: error code
 * count_coll(out): number of collations in lib associated with locale
 * lib_handle(in):
 * lf(in): locale file info
 */
int
lang_load_count_coll_from_lib (int *count_coll, void *lib_handle,
			       const LOCALE_FILE * lf)
{
  char err_msg[ERR_MSG_SIZE + PATH_MAX];
  char sym_name[SYMBOL_NAME_SIZE + 1];

  assert (count_coll != NULL);
  assert (lib_handle != NULL);
  assert (lf != NULL);
  assert (lf->locale_name != NULL);

  SHLIB_GET_VAL (*count_coll, "count_coll", int, lib_handle, lf->locale_name);

  return NO_ERROR;

error_loading_symbol:
  snprintf (err_msg, sizeof (err_msg) - 1,
	    "Cannot load symbol %s from the library file %s "
	    "for the %s locale!", sym_name, lf->lib_file, lf->locale_name);
  LOG_LOCALE_ERROR (err_msg, ER_LOC_INIT, true);

  return ER_LOC_INIT;
}

/*
 * lang_load_get_coll_name_from_lib() - reads and returns the name of n-th
 *					collation in library
 *
 * return: error code
 * coll_pos(in): collation index to return
 * coll_name(out): name of collation
 * lib_handle(in):
 * lf(in): locale file info
 */
int
lang_load_get_coll_name_from_lib (const int coll_pos, char **coll_name,
				  void *lib_handle, const LOCALE_FILE * lf)
{
  char err_msg[ERR_MSG_SIZE + PATH_MAX];
  char sym_name[SYMBOL_NAME_SIZE + 1];
  char coll_suffix[COLL_NAME_SIZE + LANG_MAX_LANGNAME + 5];

  assert (coll_name != NULL);
  assert (lib_handle != NULL);
  assert (lf != NULL);
  assert (lf->locale_name != NULL);

  *coll_name = NULL;
  snprintf (coll_suffix, sizeof (coll_suffix) - 1, "%d_%s", coll_pos,
	    lf->locale_name);
  SHLIB_GET_ADDR (*coll_name, "collation", char *, lib_handle, coll_suffix);

  return NO_ERROR;

error_loading_symbol:
  snprintf (err_msg, sizeof (err_msg) - 1,
	    "Cannot load symbol %s from the library file %s "
	    "for the %s locale!", sym_name, lf->lib_file, lf->locale_name);
  LOG_LOCALE_ERROR (err_msg, ER_LOC_INIT, true);

  return ER_LOC_INIT;
}

/*
 * lang_load_coll_from_lib() - loads collation data from library
 *
 * return: error code
 * cd(out): collation data
 * lib_handle(in):
 * lf(in): locale file info
 */
int
lang_load_coll_from_lib (COLL_DATA * cd, void *lib_handle,
			 const LOCALE_FILE * lf)
{
  char sym_name[SYMBOL_NAME_SIZE + 1];
  char *temp_char_sym;
  int *temp_num_sym;
  char err_msg[ERR_MSG_SIZE + PATH_MAX];
  int err_status = NO_ERROR;
  char *coll_checksum = NULL;

  assert (cd != NULL);
  assert (lib_handle != NULL);
  assert (lf != NULL);
  assert (lf->locale_name != NULL);

  SHLIB_GET_ADDR (temp_char_sym, "coll_name", char *, lib_handle,
		  cd->coll_name);

  if (strcmp (temp_char_sym, cd->coll_name))
    {
      err_status = ER_LOC_INIT;
      snprintf (err_msg, sizeof (err_msg) - 1,
		"Collation %s not found in shared library %s", cd->coll_name,
		lf->lib_file);
      LOG_LOCALE_ERROR (err_msg, err_status, true);
      goto exit;
    }

  SHLIB_GET_ADDR (coll_checksum, "coll_checksum", char *, lib_handle,
		  cd->coll_name);
  strncpy (cd->checksum, coll_checksum, 32);
  cd->checksum[32] = '\0';

  SHLIB_GET_VAL (cd->coll_id, "coll_id", int, lib_handle, cd->coll_name);

  SHLIB_GET_ADDR (temp_num_sym, "coll_sett_strength", int *,
		  lib_handle, cd->coll_name);
  assert (*temp_num_sym >= TAILOR_UNDEFINED &&
	  *temp_num_sym <= TAILOR_IDENTITY);
  cd->uca_opt.sett_strength = (T_LEVEL) * temp_num_sym;

  SHLIB_GET_ADDR (temp_num_sym, "coll_sett_backwards", int *,
		  lib_handle, cd->coll_name);
  cd->uca_opt.sett_backwards = (bool) * temp_num_sym;

  SHLIB_GET_ADDR (temp_num_sym, "coll_sett_caseLevel", int *,
		  lib_handle, cd->coll_name);
  cd->uca_opt.sett_caseLevel = (bool) * temp_num_sym;

  SHLIB_GET_VAL (cd->uca_opt.sett_caseFirst, "coll_sett_caseFirst", int,
		 lib_handle, cd->coll_name);

  SHLIB_GET_ADDR (temp_num_sym, "coll_sett_expansions", int *,
		  lib_handle, cd->coll_name);
  cd->uca_opt.sett_expansions = (bool) * temp_num_sym;

  SHLIB_GET_VAL (cd->uca_opt.sett_contr_policy,
		 "coll_sett_contr_policy", int, lib_handle, cd->coll_name);

  SHLIB_GET_VAL (cd->w_count, "coll_w_count", int, lib_handle, cd->coll_name);

  SHLIB_GET_VAL (cd->uca_exp_num, "coll_uca_exp_num", int, lib_handle,
		 cd->coll_name);

  SHLIB_GET_VAL (cd->count_contr, "coll_count_contr", int, lib_handle,
		 cd->coll_name);

  SHLIB_GET_ADDR (temp_num_sym, "coll_match_contr", int *, lib_handle,
		  cd->coll_name);
  cd->uca_opt.sett_match_contr = (COLL_MATCH_CONTR) * temp_num_sym;

  if (cd->count_contr > 0)
    {
      SHLIB_GET_ADDR (cd->contr_list, "coll_contr_list", COLL_CONTRACTION *,
		      lib_handle, cd->coll_name);

      SHLIB_GET_VAL (cd->contr_min_size, "coll_contr_min_size", int,
		     lib_handle, cd->coll_name);

      SHLIB_GET_VAL (cd->cp_first_contr_offset, "coll_cp_first_contr_offset",
		     int, lib_handle, cd->coll_name);

      SHLIB_GET_VAL (cd->cp_first_contr_count, "coll_cp_first_contr_count",
		     int, lib_handle, cd->coll_name);

      SHLIB_GET_ADDR (cd->cp_first_contr_array, "coll_cp_first_contr_array",
		      int *, lib_handle, cd->coll_name);
    }

  if (cd->uca_opt.sett_expansions)
    {
      assert (cd->uca_exp_num > 1);
      SHLIB_GET_ADDR (cd->uca_w_l13, "coll_uca_w_l13", UCA_L13_W *,
		      lib_handle, cd->coll_name);

      if (cd->uca_opt.sett_strength >= TAILOR_QUATERNARY)
	{
	  SHLIB_GET_ADDR (cd->uca_w_l4, "coll_uca_w_l4", UCA_L4_W *,
			  lib_handle, cd->coll_name);
	}

      SHLIB_GET_ADDR (cd->uca_num, "coll_uca_num", char *,
		      lib_handle, cd->coll_name);
    }
  else
    {
      SHLIB_GET_ADDR (cd->weights, "coll_weights", unsigned int *,
		      lib_handle, cd->coll_name);
    }

  SHLIB_GET_ADDR (cd->next_cp, "coll_next_cp", unsigned int *,
		  lib_handle, cd->coll_name);


exit:
  return err_status;

error_loading_symbol:
  snprintf (err_msg, sizeof (err_msg) - 1,
	    "Cannot load symbol %s from the library file %s "
	    "for the %s locale!", sym_name, lf->lib_file, lf->locale_name);
  LOG_LOCALE_ERROR (err_msg, ER_LOC_INIT, true);

  return ER_LOC_INIT;
}

/*
 * lang_locale_load_alpha_from_lib() - loads locale data from shared libray
 *
 * return: error code
 * a(in/out): alphabet to load
 * load_w_identifier_name(in): true if alphabet is to be load as "identifier"
 *			       name
 * lib_handle(in):
 * lf(in): locale file info
 */
static int
lang_locale_load_alpha_from_lib (ALPHABET_DATA * a,
				 bool load_w_identifier_name,
				 const char *alpha_suffix,
				 void *lib_handle, const LOCALE_FILE * lf)
{
  char sym_name[SYMBOL_NAME_SIZE + 1];
  char err_msg[ERR_MSG_SIZE + PATH_MAX];
  int err_status = NO_ERROR;

  assert (a != NULL);
  assert (lib_handle != NULL);
  assert (lf != NULL);
  assert (lf->locale_name != NULL);

  if (load_w_identifier_name)
    {
      SHLIB_GET_VAL (a->l_count, "ident_alphabet_l_count", int, lib_handle,
		     alpha_suffix);

      SHLIB_GET_VAL (a->lower_multiplier, "ident_alphabet_lower_multiplier",
		     int, lib_handle, alpha_suffix);

      SHLIB_GET_VAL (a->upper_multiplier, "ident_alphabet_upper_multiplier",
		     int, lib_handle, alpha_suffix);

      SHLIB_GET_ADDR (a->lower_cp, "ident_alphabet_lower_cp", unsigned int *,
		      lib_handle, alpha_suffix);

      SHLIB_GET_ADDR (a->upper_cp, "ident_alphabet_upper_cp", unsigned int *,
		      lib_handle, alpha_suffix);
    }
  else
    {
      SHLIB_GET_VAL (a->l_count, "alphabet_l_count", int, lib_handle,
		     alpha_suffix);

      SHLIB_GET_VAL (a->lower_multiplier, "alphabet_lower_multiplier",
		     int, lib_handle, alpha_suffix);

      SHLIB_GET_VAL (a->upper_multiplier, "alphabet_upper_multiplier",
		     int, lib_handle, alpha_suffix);

      SHLIB_GET_ADDR (a->lower_cp, "alphabet_lower_cp", unsigned int *,
		      lib_handle, alpha_suffix);

      SHLIB_GET_ADDR (a->upper_cp, "alphabet_upper_cp", unsigned int *,
		      lib_handle, alpha_suffix);
    }

  return err_status;

error_loading_symbol:
  snprintf (err_msg, sizeof (err_msg) - 1,
	    "Cannot load symbol %s from the library file %s "
	    "for the %s locale!", sym_name, lf->lib_file, lf->locale_name);
  LOG_LOCALE_ERROR (err_msg, ER_LOC_INIT, true);

  return ER_LOC_INIT;
}

/*
 * lang_load_library - loads the locale specific DLL/so
 * Returns : error code - ER_LOC_INIT if library load fails
 *			- NO_ERROR if success
 * lib_file(in)  : path to library
 * handle(out)   : handle to the loaded library
 */
int
lang_load_library (const char *lib_file, void **handle)
{
  int err_status = NO_ERROR;
  char err_msg[ERR_MSG_SIZE];
#if defined(WINDOWS)
  DWORD loading_err;
  char *lpMsgBuf;
  UINT error_mode = 0;
#else
  char *error;
#endif

  assert (lib_file != NULL);

#if defined(WINDOWS)
  error_mode = SetErrorMode (SEM_NOOPENFILEERRORBOX | SEM_FAILCRITICALERRORS);
  *handle = LoadLibrary (lib_file);
  SetErrorMode (error_mode);
  loading_err = GetLastError ();
#else
  dlerror ();			/* Clear any existing error */
  *handle = dlopen (lib_file, RTLD_NOW);
#endif

  if (*handle == NULL)
    {
      err_status = ER_LOC_INIT;
#if defined(WINDOWS)
      FormatMessage (FORMAT_MESSAGE_ALLOCATE_BUFFER
		     | FORMAT_MESSAGE_FROM_SYSTEM
		     | FORMAT_MESSAGE_ARGUMENT_ARRAY,
		     NULL,
		     loading_err,
		     MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT),
		     (char *) &lpMsgBuf, 1, &lib_file);
      snprintf (err_msg, sizeof (err_msg) - 1,
		"Library file is invalid or not accessible.\n"
		" Unable to load %s !\n %s", lib_file, lpMsgBuf);
      LocalFree (lpMsgBuf);
#else
      error = dlerror ();
      snprintf (err_msg, sizeof (err_msg) - 1,
		"Library file is invalid or not accessible.\n"
		" Unable to load %s !\n %s", lib_file, error);
#endif
      LOG_LOCALE_ERROR (err_msg, err_status, true);
    }

  return err_status;
}

/*
 * lang_unload_libraries - unloads the loaded locale libraries (DLL/so)
 *			   and frees additional data.
 */
static void
lang_unload_libraries (void)
{
  int i;

  for (i = 0; i < loclib_handle_count; i++)
    {
      assert (loclib_handle[i] != NULL);
#if defined(WINDOWS)
      FreeLibrary (loclib_handle[i]);
#else
      dlclose (loclib_handle[i]);
#endif
      loclib_handle[i] = NULL;
    }
  free (loclib_handle);
  loclib_handle = NULL;
  loclib_handle_count = 0;
}

/*
 * destroy_user_locales - frees the memory holding the locales already loaded
 *			  from the locale libraries (DLL/so)
 */
static void
destroy_user_locales (void)
{
  int i;

  for (i = 0; i < lang_count_locales; i++)
    {
      assert (lang_loaded_locales[i] != NULL);

      free_lang_locale_data (lang_loaded_locales[i]);
      lang_loaded_locales[i] = NULL;
    }

  lang_count_locales = 0;
}

/*
 * lang_locale_load_normalization_from_lib - loads normalization data from
 *					     the locale library
 */
static int
lang_locale_load_normalization_from_lib (UNICODE_NORMALIZATION * norm,
					 void *lib_handle,
					 const LOCALE_FILE * lf)
{
  char sym_name[SYMBOL_NAME_SIZE + 1];
  char err_msg[ERR_MSG_SIZE + PATH_MAX];

  assert (norm != NULL);

  memset (norm, 0, sizeof (UNICODE_NORMALIZATION));

  SHLIB_GET_ADDR (norm->unicode_mappings,
		  "unicode_mappings", UNICODE_MAPPING *,
		  lib_handle, UNICODE_NORMALIZATION_DECORATOR);
  SHLIB_GET_VAL (norm->unicode_mappings_count,
		 "unicode_mappings_count", int,
		 lib_handle, UNICODE_NORMALIZATION_DECORATOR);
  SHLIB_GET_ADDR (norm->unicode_mapping_index,
		  "unicode_mapping_index", int *,
		  lib_handle, UNICODE_NORMALIZATION_DECORATOR);
  SHLIB_GET_ADDR (norm->list_full_decomp,
		  "list_full_decomp", int *,
		  lib_handle, UNICODE_NORMALIZATION_DECORATOR);

  return NO_ERROR;

error_loading_symbol:
  snprintf (err_msg, sizeof (err_msg) - 1,
	    "Cannot load symbol %s from the library file %s "
	    "for the %s locale!", sym_name, lf->lib_file, lf->locale_name);
  LOG_LOCALE_ERROR (err_msg, ER_LOC_INIT, true);

  return ER_LOC_INIT;
}

/*
 * lang_get_generic_unicode_norm - gets the global unicode
 *		    normalization structure
 * Returns:
 */
UNICODE_NORMALIZATION *
lang_get_generic_unicode_norm (void)
{
  return generic_unicode_norm;
}

/*
 * lang_set_generic_unicode_norm - sets the global unicode
 *		    normalization structure
 */
void
lang_set_generic_unicode_norm (UNICODE_NORMALIZATION * norm)
{
  generic_unicode_norm = norm;
}

/*
 * lang_free_collations - frees all collation data
 */
static void
lang_free_collations (void)
{
  int i;

  if (lang_count_collations <= 0)
    {
      return;
    }
  for (i = 0; i < LANG_MAX_COLLATIONS; i++)
    {
      assert (lang_collations[i] != NULL);
      if (!(lang_collations[i]->built_in))
	{
	  free (lang_collations[i]);
	}
      lang_collations[i] = NULL;
    }

  lang_count_collations = 0;
}

/*
 * lang_check_coll_compat - checks compatibility of current collations (of
 *			    running process) with a reference set of
 *			    collations
 * Returns : error code
 * coll_array(in): reference collations
 * coll_cnt(in):
 * client_text(in): text to display in message error for client (this can be
 *		    "server" when checking server vs database)
 * server_text(in): text to display in message error for server (this can be
 *		    "database" when checking server vs database)
 */
int
lang_check_coll_compat (const LANG_COLL_COMPAT * coll_array,
			const int coll_cnt, const char *client_text,
			const char *server_text)
{
  char err_msg[ERR_MSG_SIZE];
  int i;
  int er_status = NO_ERROR;

  assert (coll_array != NULL);
  assert (coll_cnt > 0);
  assert (client_text != NULL);
  assert (server_text != NULL);

  if (lang_count_collations != coll_cnt)
    {
      snprintf (err_msg, sizeof (err_msg) - 1,
		"Number of collations do not match : "
		"%s has %d collations, %s has %d collations",
		client_text, lang_count_collations, server_text, coll_cnt);
      er_status = ER_LOC_INIT;
      LOG_LOCALE_ERROR (err_msg, ER_LOC_INIT, true);
      goto exit;
    }

  for (i = 0; i < coll_cnt; i++)
    {
      const LANG_COLL_COMPAT *ref_c;
      LANG_COLLATION *lc;

      ref_c = &(coll_array[i]);

      assert (ref_c->coll_id >= 0 && ref_c->coll_id < LANG_MAX_COLLATIONS);
      /* collation id is valid, check if same collation */
      lc = lang_get_collation (ref_c->coll_id);

      if (lc->coll.coll_id != ref_c->coll_id)
	{
	  snprintf (err_msg, sizeof (err_msg) - 1,
		    "Collation '%s' with id %d from %s not found with the "
		    "same id on %s", ref_c->coll_name, ref_c->coll_id,
		    server_text, client_text);
	  er_status = ER_LOC_INIT;
	  LOG_LOCALE_ERROR (err_msg, ER_LOC_INIT, true);
	  goto exit;
	}

      if (strcmp (lc->coll.coll_name, ref_c->coll_name))
	{
	  snprintf (err_msg, sizeof (err_msg) - 1,
		    "Names of collation with id %d do not match : "
		    "on %s, is '%s'; on %s, is '%s'",
		    ref_c->coll_id, client_text, ref_c->coll_name,
		    server_text, lc->coll.coll_name);
	  er_status = ER_LOC_INIT;
	  LOG_LOCALE_ERROR (err_msg, ER_LOC_INIT, true);
	  goto exit;
	}

      if (lc->codeset != ref_c->codeset)
	{
	  snprintf (err_msg, sizeof (err_msg) - 1,
		    "Codesets of collation '%s' with id %d do not match : "
		    "on %s, codeset is %d; on %s, codeset is %d",
		    ref_c->coll_name, ref_c->coll_id,
		    client_text, ref_c->codeset, server_text, lc->codeset);
	  er_status = ER_LOC_INIT;
	  LOG_LOCALE_ERROR (err_msg, ER_LOC_INIT, true);
	  goto exit;
	}

      if (strcasecmp (lc->coll.checksum, ref_c->checksum))
	{
	  snprintf (err_msg, sizeof (err_msg) - 1,
		    "Collation '%s' with id %d has changed : "
		    "on %s, checksum is '%s'; on %s, checksum is '%s'",
		    ref_c->coll_name, ref_c->coll_id,
		    client_text, ref_c->checksum,
		    server_text, lc->coll.checksum);
	  er_status = ER_LOC_INIT;
	  LOG_LOCALE_ERROR (err_msg, ER_LOC_INIT, true);
	  goto exit;
	}
    }
exit:
  return er_status;
}

/*
 * lang_check_locale_compat - checks compatibility of current locales (of
 *			      running process) with a reference set of
 *			      locales
 * Returns : error code
 * loc_array(in): reference locales
 * loc_cnt(in):
 * client_text(in): text to display in message error for client
 * server_text(in): text to display in message error for server
 */
int
lang_check_locale_compat (const LANG_LOCALE_COMPAT * loc_array,
			  const int loc_cnt, const char *client_text,
			  const char *server_text)
{
  char err_msg[ERR_MSG_SIZE];
  int i, j;
  int er_status = NO_ERROR;

  assert (loc_array != NULL);
  assert (loc_cnt > 0);

  /* check that each locale from client is defined by server */
  for (i = 0; i < lang_count_locales; i++)
    {
      LANG_LOCALE_DATA *lld = lang_loaded_locales[i];
      const LANG_LOCALE_COMPAT *ref_loc = NULL;

      do
	{
	  bool ref_found = false;

	  for (j = 0; j < loc_cnt; j++)
	    {
	      ref_loc = &(loc_array[j]);

	      if (lld->codeset == ref_loc->codeset &&
		  strcasecmp (lld->lang_name, ref_loc->lang_name) == 0)
		{
		  ref_found = true;
		  break;
		}
	    }

	  if (!ref_found)
	    {
	      snprintf (err_msg, sizeof (err_msg) - 1,
			"Locale '%s' with codeset %d loaded by %s "
			"not found on %s",
			lld->lang_name, lld->codeset, client_text,
			server_text);
	      er_status = ER_LOC_INIT;
	      LOG_LOCALE_ERROR (err_msg, ER_LOC_INIT, true);
	      goto exit;
	    }

	  assert (ref_found);

	  if (strcasecmp (ref_loc->checksum, lld->checksum))
	    {
	      snprintf (err_msg, sizeof (err_msg) - 1,
			"Locale '%s' with codeset %d has changed : "
			"on %s, checksum is '%s'; on %s, checksum is '%s'",
			ref_loc->lang_name, ref_loc->codeset,
			server_text, ref_loc->checksum,
			client_text, lld->checksum);
	      er_status = ER_LOC_INIT;
	      LOG_LOCALE_ERROR (err_msg, ER_LOC_INIT, true);
	      goto exit;
	    }
	  lld = lld->next_lld;

	}
      while (lld != NULL);
    }

  /* check that each locale from server is loaded by client */
  for (j = 0; j < loc_cnt; j++)
    {
      bool loc_found = false;
      const LANG_LOCALE_COMPAT *ref_loc = NULL;
      LANG_LOCALE_DATA *lld = NULL;

      ref_loc = &(loc_array[j]);

      for (i = 0; i < lang_count_locales && !loc_found; i++)
	{
	  lld = lang_loaded_locales[i];

	  do
	    {
	      if (lld->codeset == ref_loc->codeset &&
		  strcasecmp (lld->lang_name, ref_loc->lang_name) == 0)
		{
		  loc_found = true;
		  break;
		}
	      lld = lld->next_lld;
	    }
	  while (lld != NULL);
	}

      if (!loc_found)
	{
	  snprintf (err_msg, sizeof (err_msg) - 1,
		    "Locale '%s' with codeset %d defined on %s "
		    "is not loaded by %s",
		    ref_loc->lang_name, ref_loc->codeset, server_text,
		    client_text);
	  er_status = ER_LOC_INIT;
	  LOG_LOCALE_ERROR (err_msg, ER_LOC_INIT, true);
	  goto exit;
	}

      assert (loc_found && lld != NULL);

      if (strcasecmp (ref_loc->checksum, lld->checksum))
	{
	  snprintf (err_msg, sizeof (err_msg) - 1,
		    "Locale '%s' with codeset %d has changed : "
		    "on %s, checksum is '%s'; on %s, checksum is '%s'",
		    ref_loc->lang_name, ref_loc->codeset,
		    server_text, ref_loc->checksum,
		    client_text, lld->checksum);
	  er_status = ER_LOC_INIT;
	  LOG_LOCALE_ERROR (err_msg, ER_LOC_INIT, true);
	  goto exit;
	}
    }

exit:
  return er_status;
}
