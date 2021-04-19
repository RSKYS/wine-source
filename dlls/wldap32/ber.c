/*
 * WLDAP32 - LDAP support for Wine
 *
 * Copyright 2005 Hans Leidekker
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "winnls.h"

#include "wine/debug.h"
#include "wine/heap.h"
#include "winldap_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(wldap32);

/***********************************************************************
 *      ber_alloc_t     (WLDAP32.@)
 *
 * Allocate a berelement structure.
 *
 * PARAMS
 *  options [I] Must be LBER_USE_DER.
 *
 * RETURNS
 *  Success: Pointer to an allocated berelement structure.
 *  Failure: NULL
 *
 * NOTES
 *  Free the berelement structure with ber_free.
 */
WLDAP32_BerElement * CDECL WLDAP32_ber_alloc_t( int options )
{
    WLDAP32_BerElement *ret;

    if (!(ret = heap_alloc( sizeof(*ret) ))) return NULL;
    if (!(ret->opaque = ldap_funcs->ber_alloc_t( options )))
    {
        heap_free( ret );
        return NULL;
    }
    return ret;
}


/***********************************************************************
 *      ber_bvdup     (WLDAP32.@)
 *
 * Copy a berval structure.
 *
 * PARAMS
 *  berval [I] Pointer to the berval structure to be copied.
 *
 * RETURNS
 *  Success: Pointer to a copy of the berval structure.
 *  Failure: NULL
 *
 * NOTES
 *  Free the copy with ber_bvfree.
 */
BERVAL * CDECL WLDAP32_ber_bvdup( BERVAL *berval )
{
    return bervalWtoW( berval );
}


/***********************************************************************
 *      ber_bvecfree     (WLDAP32.@)
 *
 * Free an array of berval structures.
 *
 * PARAMS
 *  berval [I] Pointer to an array of berval structures.
 *
 * RETURNS
 *  Nothing.
 *
 * NOTES
 *  Use this function only to free an array of berval structures
 *  returned by a call to ber_scanf with a 'V' in the format string.
 */
void CDECL WLDAP32_ber_bvecfree( BERVAL **berval )
{
    bvarrayfreeW( berval );
}


/***********************************************************************
 *      ber_bvfree     (WLDAP32.@)
 *
 * Free a berval structure.
 *
 * PARAMS
 *  berval [I] Pointer to a berval structure.
 *
 * RETURNS
 *  Nothing.
 *
 * NOTES
 *  Use this function only to free berval structures allocated by
 *  an LDAP API.
 */
void CDECL WLDAP32_ber_bvfree( BERVAL *berval )
{
    heap_free( berval );
}


/***********************************************************************
 *      ber_first_element     (WLDAP32.@)
 *
 * Return the tag of the first element in a set or sequence.
 *
 * PARAMS
 *  berelement [I] Pointer to a berelement structure.
 *  len        [O] Receives the length of the first element.
 *  opaque     [O] Receives a pointer to a cookie.
 *
 * RETURNS
 *  Success: Tag of the first element.
 *  Failure: LBER_DEFAULT (no more data).
 *
 * NOTES
 *  len and cookie should be passed to ber_next_element.
 */
ULONG CDECL WLDAP32_ber_first_element( WLDAP32_BerElement *ber, ULONG *len, char **opaque )
{
    return ldap_funcs->ber_first_element( ber->opaque, len, opaque );
}


/***********************************************************************
 *      ber_flatten     (WLDAP32.@)
 *
 * Flatten a berelement structure into a berval structure.
 *
 * PARAMS
 *  berelement [I] Pointer to a berelement structure.
 *  berval    [O] Pointer to a berval structure.
 *
 * RETURNS
 *  Success: 0
 *  Failure: LBER_ERROR
 *
 * NOTES
 *  Free the berval structure with ber_bvfree.
 */
int CDECL WLDAP32_ber_flatten( WLDAP32_BerElement *ber, BERVAL **berval )
{
    struct bervalU *bervalU;
    struct WLDAP32_berval *bervalW;

    if (ldap_funcs->ber_flatten( ber->opaque, &bervalU )) return WLDAP32_LBER_ERROR;

    if (!(bervalW = bervalUtoW( bervalU ))) return WLDAP32_LBER_ERROR;
    ldap_funcs->ber_bvfree( bervalU );
    if (!bervalW) return WLDAP32_LBER_ERROR;
    *berval = bervalW;
    return 0;
}


/***********************************************************************
 *      ber_free     (WLDAP32.@)
 *
 * Free a berelement structure.
 *
 * PARAMS
 *  berelement [I] Pointer to the berelement structure to be freed.
 *  buf       [I] Flag.
 *
 * RETURNS
 *  Nothing.
 *
 * NOTES
 *  Set buf to 0 if the berelement was allocated with ldap_first_attribute
 *  or ldap_next_attribute, otherwise set it to 1.
 */
void CDECL WLDAP32_ber_free( WLDAP32_BerElement *ber, int freebuf )
{
    ldap_funcs->ber_free( ber->opaque, freebuf );
    heap_free( ber );
}


/***********************************************************************
 *      ber_init     (WLDAP32.@)
 *
 * Initialise a berelement structure from a berval structure.
 *
 * PARAMS
 *  berval [I] Pointer to a berval structure.
 *
 * RETURNS
 *  Success: Pointer to a berelement structure.
 *  Failure: NULL
 *
 * NOTES
 *  Call ber_free to free the returned berelement structure.
 */
WLDAP32_BerElement * CDECL WLDAP32_ber_init( BERVAL *berval )
{
    struct bervalU *bervalU;
    WLDAP32_BerElement *ret;

    if (!(ret = heap_alloc( sizeof(*ret) ))) return NULL;
    if (!(bervalU = bervalWtoU( berval )))
    {
        heap_free( ret );
        return NULL;
    }
    if (!(ret->opaque = ldap_funcs->ber_init( bervalU )))
    {
        heap_free( ret );
        ret = NULL;
    }
    heap_free( bervalU );
    return ret;
}


/***********************************************************************
 *      ber_next_element     (WLDAP32.@)
 *
 * Return the tag of the next element in a set or sequence.
 *
 * PARAMS
 *  berelement [I]   Pointer to a berelement structure.
 *  len        [I/O] Receives the length of the next element.
 *  opaque     [I/O] Pointer to a cookie.
 *
 * RETURNS
 *  Success: Tag of the next element.
 *  Failure: LBER_DEFAULT (no more data).
 *
 * NOTES
 *  len and cookie are initialized by ber_first_element and should
 *  be passed on in subsequent calls to ber_next_element.
 */
ULONG CDECL WLDAP32_ber_next_element( WLDAP32_BerElement *ber, ULONG *len, char *opaque )
{
    return ldap_funcs->ber_next_element( ber->opaque, len, opaque );
}


/***********************************************************************
 *      ber_peek_tag     (WLDAP32.@)
 *
 * Return the tag of the next element.
 *
 * PARAMS
 *  berelement [I] Pointer to a berelement structure.
 *  len        [O] Receives the length of the next element.
 *
 * RETURNS
 *  Success: Tag of the next element.
 *  Failure: LBER_DEFAULT (no more data).
 */
ULONG CDECL WLDAP32_ber_peek_tag( WLDAP32_BerElement *ber, ULONG *len )
{
    return ldap_funcs->ber_peek_tag( ber->opaque, len );
}


/***********************************************************************
 *      ber_skip_tag     (WLDAP32.@)
 *
 * Skip the current tag and return the tag of the next element.
 *
 * PARAMS
 *  berelement [I] Pointer to a berelement structure.
 *  len        [O] Receives the length of the skipped element.
 *
 * RETURNS
 *  Success: Tag of the next element.
 *  Failure: LBER_DEFAULT (no more data).
 */
ULONG CDECL WLDAP32_ber_skip_tag( WLDAP32_BerElement *ber, ULONG *len )
{
    return ldap_funcs->ber_skip_tag( ber->opaque, len );
}


/***********************************************************************
 *      ber_printf     (WLDAP32.@)
 *
 * Encode a berelement structure.
 *
 * PARAMS
 *  berelement [I/O] Pointer to a berelement structure.
 *  fmt        [I]   Format string.
 *  ...        [I]   Values to encode.
 *
 * RETURNS
 *  Success: Non-negative number.
 *  Failure: LBER_ERROR
 *
 * NOTES
 *  berelement must have been allocated with ber_alloc_t. This function
 *  can be called multiple times to append data.
 */
int WINAPIV WLDAP32_ber_printf( WLDAP32_BerElement *ber, char *fmt, ... )
{
    __ms_va_list list;
    int ret = 0;
    char new_fmt[2];

    new_fmt[1] = 0;
    __ms_va_start( list, fmt );
    while (*fmt)
    {
        new_fmt[0] = *fmt++;
        switch (new_fmt[0])
        {
        case 'b':
        case 'e':
        case 'i':
        {
            int i = va_arg( list, int );
            ret = ldap_funcs->ber_printf( ber->opaque, new_fmt, i );
            break;
        }
        case 'o':
        case 's':
        {
            char *str = va_arg( list, char * );
            ret = ldap_funcs->ber_printf( ber->opaque, new_fmt, str );
            break;
        }
        case 't':
        {
            unsigned int tag = va_arg( list, unsigned int );
            ret = ldap_funcs->ber_printf( ber->opaque, new_fmt, tag );
            break;
        }
        case 'v':
        {
            char **array = va_arg( list, char ** );
            ret = ldap_funcs->ber_printf( ber->opaque, new_fmt, array );
            break;
        }
        case 'V':
        {
            struct WLDAP32_berval **array = va_arg( list, struct WLDAP32_berval ** );
            struct bervalU **arrayU;
            if (!(arrayU = bvarrayWtoU( array )))
            {
                ret = -1;
                break;
            }
            ret = ldap_funcs->ber_printf( ber->opaque, new_fmt, arrayU );
            bvarrayfreeU( arrayU );
            break;
        }
        case 'X':
        {
            char *str = va_arg( list, char * );
            int len = va_arg( list, int );
            new_fmt[0] = 'B';  /* 'X' is deprecated */
            ret = ldap_funcs->ber_printf( ber->opaque, new_fmt, str, len );
            break;
        }
        case 'n':
        case '{':
        case '}':
        case '[':
        case ']':
            ret = ldap_funcs->ber_printf( ber->opaque, new_fmt );
            break;

        default:
            FIXME( "Unknown format '%c'\n", new_fmt[0] );
            ret = -1;
            break;
        }
        if (ret == -1) break;
    }
    __ms_va_end( list );
    return ret;
}


/***********************************************************************
 *      ber_scanf     (WLDAP32.@)
 *
 * Decode a berelement structure.
 *
 * PARAMS
 *  berelement [I/O] Pointer to a berelement structure.
 *  fmt        [I]   Format string.
 *  ...        [I]   Pointers to values to be decoded.
 *
 * RETURNS
 *  Success: Non-negative number.
 *  Failure: LBER_ERROR
 *
 * NOTES
 *  berelement must have been allocated with ber_init. This function
 *  can be called multiple times to decode data.
 */
ULONG WINAPIV WLDAP32_ber_scanf( WLDAP32_BerElement *ber, char *fmt, ... )
{
    __ms_va_list list;
    int ret = 0;
    char new_fmt[2];

    new_fmt[1] = 0;
    __ms_va_start( list, fmt );
    while (*fmt)
    {
        new_fmt[0] = *fmt++;
        switch (new_fmt[0])
        {
        case 'a':
        {
            char *str, **ptr = va_arg( list, char ** );
            if ((ret = ldap_funcs->ber_scanf( ber->opaque, new_fmt, &str )) == -1) break;
            *ptr = strdupU( str );
            ldap_funcs->ldap_memfree( str );
            break;
        }
        case 'b':
        case 'e':
        case 'i':
        {
            int *i = va_arg( list, int * );
            ret = ldap_funcs->ber_scanf( ber->opaque, new_fmt, i );
            break;
        }
        case 't':
        {
            unsigned int *tag = va_arg( list, unsigned int * );
            ret = ldap_funcs->ber_scanf( ber->opaque, new_fmt, tag );
            break;
        }
        case 'v':
        {
            char *str, **arrayU, **ptr, ***array = va_arg( list, char *** );
            if ((ret = ldap_funcs->ber_scanf( ber->opaque, new_fmt, &arrayU )) == -1) break;
            *array = strarrayUtoU( arrayU );
            ptr = arrayU;
            while ((str = *ptr))
            {
                ldap_funcs->ldap_memfree( str );
                ptr++;
            }
            ldap_funcs->ldap_memfree( arrayU );
            break;
        }
        case 'B':
        {
            char *strU, **str = va_arg( list, char ** );
            int *len = va_arg( list, int * );
            if ((ret = ldap_funcs->ber_scanf( ber->opaque, new_fmt, &strU, len )) == -1) break;
            *str = heap_alloc( *len );
            memcpy( *str, strU, *len );
            ldap_funcs->ldap_memfree( strU );
            break;
        }
        case 'O':
        {
            struct WLDAP32_berval **berval = va_arg( list, struct WLDAP32_berval ** );
            struct bervalU *bervalU;
            if ((ret = ldap_funcs->ber_scanf( ber->opaque, new_fmt, &bervalU )) == -1) break;
            *berval = bervalUtoW( bervalU );
            ldap_funcs->ber_bvfree( bervalU );
            break;
        }
        case 'V':
        {
            struct WLDAP32_berval ***array = va_arg( list, struct WLDAP32_berval *** );
            struct bervalU **arrayU;
            if ((ret = ldap_funcs->ber_scanf( ber->opaque, new_fmt, &arrayU )) == -1) break;
            *array = bvarrayUtoW( arrayU );
            ldap_funcs->ber_bvecfree( arrayU );
            break;
        }
        case 'n':
        case 'x':
        case '{':
        case '}':
        case '[':
        case ']':
            ret = ldap_funcs->ber_scanf( ber->opaque, new_fmt );
            break;

        default:
            FIXME( "Unknown format '%c'\n", new_fmt[0] );
            ret = -1;
            break;
        }
        if (ret == -1) break;
    }
    __ms_va_end( list );
    return ret;
}
