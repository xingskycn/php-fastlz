
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "php_scandir.h"
#include "zend_globals.h"
#include "zend_ini_scanner.h"
#include "zend_hash.h"
#include "ext/standard/info.h"
#include "SAPI.h"
#ifdef PHP_WIN32
# include "win32/php_stdint.h"
#else
# include "stdint.h"
#endif

#ifdef HAVE_APC_SUPPORT
# include "ext/standard/php_var.h"
# include "ext/standard/php_smart_str.h"
# include "ext/apc/apc_serializer.h"
#endif

#include "php_fastlz.h"
#include "libfastlz/fastlz.h"

/* {{{ fastlz globals 
 *
 * true globals, no need for thread safety here 
 */
/* }}} */

/* {{{ PHP_FUNCTION declarations */
PHP_FUNCTION(fastlz_compress);
PHP_FUNCTION(fastlz_decompress);
/* }}} */

/* {{{ ZEND_DECLARE_MODULE_GLOBALS(fastlz) */
ZEND_DECLARE_MODULE_GLOBALS(fastlz)
/* }}} */

/* {{{ php_fastlz_init_globals */
static void php_fastlz_init_globals(zend_fastlz_globals* fastlz_globals TSRMLS_DC)
{
}
/* }}} */

/* {{{ php_fastlz_shutdown_globals */
static void php_fastlz_shutdown_globals(zend_fastlz_globals* fastlz_globals TSRMLS_DC)
{
	/* nothing ? */
}
/* }}} */

/* {{{ ini entries */
PHP_INI_BEGIN()
STD_PHP_INI_ENTRY("fastlz.compression", "1",  PHP_INI_SYSTEM, OnUpdateLong,    compression_level,   zend_fastlz_globals, fastlz_globals)
PHP_INI_END()
/* }}} */

/* {{{ arginfo static macro */
#if PHP_MAJOR_VERSION > 5 || PHP_MAJOR_VERSION == 5 && PHP_MINOR_VERSION >= 3
#define FASTLZ_ARGINFO_STATIC
#else
#define FASTLZ_ARGINFO_STATIC static
#endif
/* }}} */

/* {{{ arginfo */
FASTLZ_ARGINFO_STATIC
ZEND_BEGIN_ARG_INFO(php_fastlz_compress_arginfo, 0)
    ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

FASTLZ_ARGINFO_STATIC
ZEND_BEGIN_ARG_INFO(php_fastlz_decompress_arginfo, 0)
    ZEND_ARG_INFO(0, compressed)
ZEND_END_ARG_INFO()
/* }}} */


/* {{{ fastlz_functions[]
 *
 * Every user visible function must have an entry in fastlz_functions[].
 */
zend_function_entry fastlz_functions[] = {
	PHP_FE(fastlz_compress,               php_fastlz_compress_arginfo)
	PHP_FE(fastlz_decompress,             php_fastlz_decompress_arginfo)
	{NULL, NULL, NULL}	/* Must be the last line in fastlz_functions[] */
};
/* }}} */

/* {{{ fastlz_module_entry
 */
zend_module_entry fastlz_module_entry = {
	STANDARD_MODULE_HEADER,
	"fastlz",
	fastlz_functions,
	PHP_MINIT(fastlz),
	PHP_MSHUTDOWN(fastlz),
	NULL,
	NULL,
	PHP_MINFO(fastlz),
#if ZEND_MODULE_API_NO >= 20010901
	PHP_FASTLZ_VERSION,
#endif
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_FASTLZ
ZEND_GET_MODULE(fastlz)
#endif

/* {{{ fastlz helper functions */
static int fastlz_xcompress(char *value, int value_len, char** cvalue TSRMLS_DC)
{
	uint32_t compressed_len;
	char *compressed;

	assert(value && cvalue);

	compressed_len = sizeof(uint32_t) + ((value_len*1.05) + 1);

	compressed = emalloc(compressed_len);

	if(compressed)
	{
		memcpy(compressed, &value_len, sizeof(uint32_t));
		compressed += sizeof(uint32_t);
		compressed_len = fastlz_compress_level(FASTLZ_G(compression_level), value, value_len, compressed);
		if(compressed_len > 0)
		{
			compressed_len += sizeof(uint32_t);
			compressed -= sizeof(uint32_t);
			(*cvalue) = compressed;
			return compressed_len;
		}
		else
		{
			efree(compressed);
		}
	}

	return 0;
}

static int fastlz_xdecompress(char *compressed, int compressed_len, char** uvalue TSRMLS_DC)
{
	char *value;
	uint32_t value_len;

	assert(compressed || uvalue);

	if(compressed_len > sizeof(uint32_t)) 
	{
		memcpy(&value_len, compressed, sizeof(uint32_t));
		if(value_len > 0) 
		{
			compressed += sizeof(uint32_t);
			compressed_len -= sizeof(uint32_t);
			value = emalloc(value_len);
			if(value)
			{
				if(value_len == fastlz_decompress(compressed, compressed_len, value, value_len)) 
				{
					(*uvalue) = value;
					return value_len;
				}
				else
				{
					efree(value);
				}
			}
		}
	}

	return 0;
}
/* }}} */

#ifdef HAVE_APC_SUPPORT
/* {{{ apc php serializers */
int APC_SERIALIZER_NAME(fastlz) (APC_SERIALIZER_ARGS) 
{
    smart_str strbuf = {0};
    php_serialize_data_t var_hash;
    PHP_VAR_SERIALIZE_INIT(var_hash);
    php_var_serialize(&strbuf, (zval**)&value, &var_hash TSRMLS_CC);
    PHP_VAR_SERIALIZE_DESTROY(var_hash);
    if(strbuf.c) 
	{
		*buf_len = fastlz_xcompress(strbuf.c, strbuf.len, (char**)buf);

		smart_str_free(&strbuf);

		if(*buf_len > 0) 
		{
			return 1;
		}
    }
    return 0;
}

int APC_UNSERIALIZER_NAME(fastlz) (APC_UNSERIALIZER_ARGS) 
{
    unsigned char *ubuf = NULL;
	int ubuf_len = 0;
	const unsigned char* tmp;
    php_unserialize_data_t var_hash;
    PHP_VAR_UNSERIALIZE_INIT(var_hash);

	ubuf_len = fastlz_xdecompress(buf, buf_len, (char**)&ubuf);

	if(ubuf_len > 0 && ubuf) 
	{
		tmp = ubuf;
		if(!php_var_unserialize(value, &tmp, ubuf + ubuf_len, &var_hash TSRMLS_CC)) {
			PHP_VAR_UNSERIALIZE_DESTROY(var_hash);
			zval_dtor(*value);
			php_error_docref(NULL TSRMLS_CC, E_NOTICE, "Error unserializing at %ld offset of %ld bytes", (long)(tmp - ubuf), (long)buf_len);
			(*value)->type = IS_NULL;
			efree(ubuf);
			return 0;
		}
		efree(ubuf);
	}
    PHP_VAR_UNSERIALIZE_DESTROY(var_hash);
    return 1;
}
/* }}} */
#endif

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(fastlz)
{
	ZEND_INIT_MODULE_GLOBALS(fastlz, php_fastlz_init_globals, php_fastlz_shutdown_globals);

	REGISTER_INI_ENTRIES();

#if HAVE_APC_SUPPORT 
    apc_register_serializer("php+fastlz", APC_SERIALIZER_NAME(fastlz), APC_UNSERIALIZER_NAME(fastlz), NULL TSRMLS_CC);
#endif
	
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 *  */
PHP_MSHUTDOWN_FUNCTION(fastlz)
{
#ifdef ZTS
	ts_free_id(fastlz_globals_id);
#else
	php_fastlz_shutdown_globals(&fastlz_globals);
#endif

	UNREGISTER_INI_ENTRIES();
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(fastlz)
{
	int module_number = zend_module->module_number;
	
	php_info_print_table_start();

	php_info_print_table_header(2, "fastlz support", "enabled");
	php_info_print_table_row(2, "version", PHP_FASTLZ_VERSION);
	php_info_print_table_end();
}
/* }}} */

/* {{{ proto string fastlz_compress(string value)
 */
PHP_FUNCTION(fastlz_compress) 
{
	char *value;
	int value_len;
	uint32_t compressed_len;
	char *compressed;

	if(zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &value, &value_len) == FAILURE)
	{
		return;
	}

	compressed_len = fastlz_xcompress(value, value_len, &compressed);

	if(compressed_len > 0)
	{
		RETURN_STRINGL(compressed, compressed_len, 0);
		return;
	}

	RETURN_NULL();
}
/* }}} */

/* {{{ proto string fastlz_decompress(string key)
 */
PHP_FUNCTION(fastlz_decompress) 
{
	char *value = NULL;
	uint32_t value_len = 0;
	int compressed_len;
	char *compressed;

	if(zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &compressed, &compressed_len) == FAILURE)
	{
		return;
	}

	value_len = fastlz_xdecompress(compressed, compressed_len, &value);

	if(value_len > 0) 
	{
		RETURN_STRINGL(value, value_len, 0);
		return;
	}

	RETURN_NULL();
}
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim>600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
