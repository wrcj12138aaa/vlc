/*****************************************************************************
 * http.c: HTTP input module
 *****************************************************************************
 * Copyright (C) 2001-2004 VideoLAN
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Christophe Massiot <massiot@via.ecp.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <stdlib.h>

#include <vlc/vlc.h>
#include <vlc/input.h>

#include "vlc_playlist.h"
#include "network.h"

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

#define PROXY_TEXT N_("HTTP proxy")
#define PROXY_LONGTEXT N_( \
    "You can specify an HTTP proxy to use. It must be of the form " \
    "http://myproxy.mydomain:myport/. If none is specified, the HTTP_PROXY " \
    "environment variable will be tried." )

#define CACHING_TEXT N_("Caching value in ms")
#define CACHING_LONGTEXT N_( \
    "Allows you to modify the default caching value for http streams. This " \
    "value should be set in millisecond units." )

#define USER_TEXT N_("HTTP user name")
#define USER_LONGTEXT N_("Allows you to modify the user name that will " \
    "be used for the connection (Basic authentication only).")

#define PASS_TEXT N_("HTTP password")
#define PASS_LONGTEXT N_("Allows you to modify the password that will be " \
    "used for the connection.")

#define AGENT_TEXT N_("HTTP user agent")
#define AGENT_LONGTEXT N_("Allows you to modify the user agent that will be " \
    "used for the connection.")

vlc_module_begin();
    set_description( _("HTTP input") );
    set_capability( "access2", 0 );

    add_string( "http-proxy", NULL, NULL, PROXY_TEXT, PROXY_LONGTEXT,
                VLC_FALSE );
    add_integer( "http-caching", 4 * DEFAULT_PTS_DELAY / 1000, NULL,
                 CACHING_TEXT, CACHING_LONGTEXT, VLC_TRUE );
    add_string( "http-user", NULL, NULL, USER_TEXT, USER_LONGTEXT, VLC_FALSE );
    add_string( "http-pwd", NULL , NULL, PASS_TEXT, PASS_LONGTEXT, VLC_FALSE );
    add_string( "http-user-agent", COPYRIGHT_MESSAGE , NULL, AGENT_TEXT,
                AGENT_LONGTEXT, VLC_FALSE );

    add_shortcut( "http" );
    add_shortcut( "http4" );
    add_shortcut( "http6" );
    set_callbacks( Open, Close );
vlc_module_end();

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
struct access_sys_t
{
    int fd;

    /* From uri */
    vlc_url_t url;
    char    *psz_user;
    char    *psz_passwd;
    char    *psz_user_agent;

    /* Proxy */
    vlc_bool_t b_proxy;
    vlc_url_t  proxy;

    /* */
    int        i_code;
    char      *psz_protocol;
    int        i_version;

    char       *psz_mime;
    char       *psz_location;

    vlc_bool_t b_chunked;
    int64_t    i_chunk;
    int64_t    i_tell;
    int64_t    i_size;

    vlc_bool_t b_seekable;
    vlc_bool_t b_eof;
};

/* */
static int Read( access_t *, uint8_t *, int );
static int Seek( access_t *, int64_t );
static int Control( access_t *, int, va_list );

/* */
static void ParseURL( access_sys_t *, char *psz_url );
static int  Connect( access_t *, int64_t );
static char *b64_encode( unsigned char *src );

/*****************************************************************************
 * Open:
 *****************************************************************************/
static int  Open ( vlc_object_t *p_this )
{
    access_t     *p_access = (access_t*)p_this;
    access_sys_t *p_sys;
    vlc_value_t   val;

    /* Create private struct */
    p_sys = malloc( sizeof( access_sys_t ) );
    memset( p_sys, 0, sizeof( access_sys_t ) );
    p_sys->fd = -1;
    p_sys->b_proxy = VLC_FALSE;
    p_sys->i_version = 1;
    p_sys->b_seekable = VLC_TRUE;
    p_sys->psz_mime = NULL;
    p_sys->psz_location = NULL;
    p_sys->psz_user_agent = NULL;
    p_sys->b_eof = VLC_FALSE;

    /* First set ipv4/ipv6 */
    var_Create( p_access, "ipv4", VLC_VAR_BOOL | VLC_VAR_DOINHERIT );
    var_Create( p_access, "ipv6", VLC_VAR_BOOL | VLC_VAR_DOINHERIT );

    if( *p_access->psz_access )
    {
        /* Find out which shortcut was used */
        if( !strncmp( p_access->psz_access, "http4", 6 ) )
        {
            val.b_bool = VLC_TRUE;
            var_Set( p_access, "ipv4", val );

            val.b_bool = VLC_FALSE;
            var_Set( p_access, "ipv6", val );
        }
        else if( !strncmp( p_access->psz_access, "http6", 6 ) )
        {
            val.b_bool = VLC_TRUE;
            var_Set( p_access, "ipv6", val );

            val.b_bool = VLC_FALSE;
            var_Set( p_access, "ipv4", val );
        }
    }

    /* Pts delay */
    var_Create( p_access, "http-caching", VLC_VAR_INTEGER | VLC_VAR_DOINHERIT );

    /* Parse URI */
    ParseURL( p_sys, p_access->psz_path );
    if( p_sys->url.psz_host == NULL || *p_sys->url.psz_host == '\0' )
    {
        msg_Warn( p_access, "invalid host" );
        goto error;
    }
    if( p_sys->url.i_port <= 0 )
    {
        p_sys->url.i_port = 80;
    }
    if( !p_sys->psz_user || *p_sys->psz_user == '\0' )
    {
        var_Create( p_access, "http-user", VLC_VAR_STRING | VLC_VAR_DOINHERIT );
        var_Get( p_access, "http-user", &val );
        p_sys->psz_user = val.psz_string;

        var_Create( p_access, "http-pwd", VLC_VAR_STRING | VLC_VAR_DOINHERIT );
        var_Get( p_access, "http-pwd", &val );
        p_sys->psz_passwd = val.psz_string;
    }

    /* Do user agent */
    var_Create( p_access, "http-user-agent", VLC_VAR_STRING | VLC_VAR_DOINHERIT );
    var_Get( p_access, "http-user-agent", &val );
    p_sys->psz_user_agent = val.psz_string;

    /* Check proxy */
    var_Create( p_access, "http-proxy", VLC_VAR_STRING | VLC_VAR_DOINHERIT );
    var_Get( p_access, "http-proxy", &val );
    if( val.psz_string && *val.psz_string )
    {
        p_sys->b_proxy = VLC_TRUE;
        vlc_UrlParse( &p_sys->proxy, val.psz_string, 0 );
    }
    else
    {
        char *psz_proxy = getenv( "http_proxy" );
        if( psz_proxy && *psz_proxy )
        {
            p_sys->b_proxy = VLC_TRUE;
            vlc_UrlParse( &p_sys->proxy, val.psz_string, 0 );
        }
        if( psz_proxy )
        {
            free( psz_proxy );
        }
    }
    if( val.psz_string )
    {
        free( val.psz_string );
    }

    if( p_sys->b_proxy )
    {
        if( p_sys->proxy.psz_host == NULL || *p_sys->proxy.psz_host == '\0' )
        {
            msg_Warn( p_access, "invalid proxy host" );
            goto error;
        }
        if( p_sys->proxy.i_port <= 0 )
        {
            p_sys->proxy.i_port = 80;
        }
    }

    msg_Dbg( p_access, "http: server='%s' port=%d file='%s",
             p_sys->url.psz_host, p_sys->url.i_port, p_sys->url.psz_path );
    if( p_sys->b_proxy )
    {
        msg_Dbg( p_access, "      proxy %s:%d", p_sys->proxy.psz_host,
                 p_sys->proxy.i_port );
    }
    if( p_sys->psz_user && *p_sys->psz_user )
    {
        msg_Dbg( p_access, "      user='%s', pwd='%s'",
                 p_sys->psz_user, p_sys->psz_passwd );
    }

    /* Connect */
    p_access->p_sys = p_sys;
    if( Connect( p_access, 0 ) )
    {
        /* Retry with http 1.0 */
        p_sys->i_version = 0;

        if( p_access->b_die ||
            Connect( p_access, 0 ) )
        {
            goto error;
        }
    }

    if( ( p_sys->i_code == 301 || p_sys->i_code == 302 ||
          p_sys->i_code == 303 || p_sys->i_code == 307 ) &&
        p_sys->psz_location && *p_sys->psz_location )
    {
        playlist_t * p_playlist;

        msg_Dbg( p_access, "redirection to %s", p_sys->psz_location );

        p_playlist = vlc_object_find( p_access, VLC_OBJECT_PLAYLIST, FIND_ANYWHERE );
        if( !p_playlist )
        {
            msg_Err( p_access, "redirection failed: can't find playlist" );
            goto error;
        }
        p_playlist->pp_items[p_playlist->i_index]->b_autodeletion = VLC_TRUE;
        playlist_Add( p_playlist, p_sys->psz_location, p_sys->psz_location,
                      PLAYLIST_INSERT | PLAYLIST_GO,
                      p_playlist->i_index + 1 );
        vlc_object_release( p_playlist );

        p_sys->i_size = 0;  /* Force to stop reading */
    }

    /* Set up p_access */
    p_access->pf_read = Read;
    p_access->pf_block = NULL;
    p_access->pf_control = Control;
    p_access->pf_seek = Seek;
    p_access->p_sys = p_sys;
    if( !strcmp( p_sys->psz_protocol, "ICY" ) )
    {
        if( p_sys->psz_mime && !strcasecmp( p_sys->psz_mime, "video/nsv" ) )
            p_access->psz_demux = strdup( "nsv" );
        else
            p_access->psz_demux = strdup( "mp3" );

        msg_Info( p_access, "ICY server found, %s demuxer selected",
                  p_access->psz_demux );
    }
    return VLC_SUCCESS;

error:
    vlc_UrlClean( &p_sys->url );
    vlc_UrlClean( &p_sys->proxy );
    if( p_sys->psz_mime ) free( p_sys->psz_mime );
    if( p_sys->psz_location ) free( p_sys->psz_location );
    if( p_sys->psz_user_agent ) free( p_sys->psz_user_agent );
    if( p_sys->psz_user ) free( p_sys->psz_user );
    if( p_sys->psz_passwd ) free( p_sys->psz_passwd );

    if( p_sys->fd > 0 )
    {
        net_Close( p_sys->fd );
    }
    free( p_sys );
    return VLC_EGENERIC;
}

/*****************************************************************************
 * Close:
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    access_t     *p_access = (access_t*)p_this;
    access_sys_t *p_sys = p_access->p_sys;

    vlc_UrlClean( &p_sys->url );
    vlc_UrlClean( &p_sys->proxy );

    if( p_sys->psz_user ) free( p_sys->psz_user );
    if( p_sys->psz_passwd ) free( p_sys->psz_passwd );

    if( p_sys->psz_mime ) free( p_sys->psz_mime );
    if( p_sys->psz_location ) free( p_sys->psz_location );

    if( p_sys->psz_user_agent ) free( p_sys->psz_user_agent );

    if( p_sys->fd > 0 )
    {
        net_Close( p_sys->fd );
    }
    free( p_sys );
}

/*****************************************************************************
 * Read: Read up to i_len bytes from the http connection and place in
 * p_buffer. Return the actual number of bytes read
 *****************************************************************************/
static int Read( access_t *p_access, uint8_t *p_buffer, int i_len )
{
    access_sys_t *p_sys   = p_access->p_sys;
    int          i_read;

    if( p_sys->fd < 0 )
    {
        p_sys->b_eof = VLC_TRUE;
        return 0;
    }

    if( p_sys->i_size > 0 && i_len + p_sys->i_tell > p_sys->i_size )
    {
        if( ( i_len = p_sys->i_size - p_sys->i_tell ) == 0 )
        {
            p_sys->b_eof = VLC_TRUE;
            return 0;
        }
    }
    if( p_sys->b_chunked )
    {
        if( p_sys->i_chunk < 0 )
        {
            p_sys->b_eof = VLC_TRUE;
            return 0;
        }

        if( p_sys->i_chunk <= 0 )
        {
            char *psz = net_Gets( VLC_OBJECT(p_access), p_sys->fd );
            /* read the chunk header */
            if( psz == NULL )
            {
                msg_Dbg( p_access, "failed reading chunk-header line" );
                return -1;
            }
            p_sys->i_chunk = strtoll( psz, NULL, 16 );
            free( psz );

            if( p_sys->i_chunk <= 0 )   /* eof */
            {
                p_sys->i_chunk = -1;
                p_sys->b_eof = VLC_TRUE;
                return 0;
            }
        }

        if( i_len > p_sys->i_chunk )
        {
            i_len = p_sys->i_chunk;
        }
    }


    i_read = net_Read( p_access, p_sys->fd, p_buffer, i_len, VLC_FALSE );
    if( i_read > 0 )
    {
        p_sys->i_tell += i_read;

        if( p_sys->b_chunked )
        {
            p_sys->i_chunk -= i_read;
            if( p_sys->i_chunk <= 0 )
            {
                /* read the empty line */
                char *psz = net_Gets( VLC_OBJECT(p_access), p_sys->fd );
                if( psz )
                {
                    free( psz );
                }
            }
        }
    }
    else if( i_read == 0 )
    {
        p_sys->b_eof = VLC_TRUE;
    }
    return i_read;
}

/*****************************************************************************
 * Seek: close and re-open a connection at the right place
 *****************************************************************************/
static int Seek( access_t *p_access, int64_t i_pos )
{
    access_sys_t *p_sys = p_access->p_sys;

    msg_Dbg( p_access, "trying to seek to "I64Fd, i_pos );

    net_Close( p_sys->fd ); p_sys->fd = -1;

    if( Connect( p_access, i_pos ) )
    {
        msg_Err( p_access, "seek failed" );
        p_sys->b_eof = VLC_TRUE;
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Control:
 *****************************************************************************/
static int Control( access_t *p_access, int i_query, va_list args )
{
    access_sys_t *p_sys = p_access->p_sys;
    vlc_bool_t   *pb_bool;
    int          *pi_int;
    int64_t      *pi_64;
    vlc_value_t  val;

    switch( i_query )
    {
        /* */
        case ACCESS_CAN_SEEK:
            pb_bool = (vlc_bool_t*)va_arg( args, vlc_bool_t* );
            *pb_bool = p_sys->b_seekable;
            break;
        case ACCESS_CAN_FASTSEEK:
            pb_bool = (vlc_bool_t*)va_arg( args, vlc_bool_t* );
            *pb_bool = VLC_FALSE;
            break;
        case ACCESS_CAN_PAUSE:
            pb_bool = (vlc_bool_t*)va_arg( args, vlc_bool_t* );
            *pb_bool = VLC_TRUE;    /* FIXME */
            break;
        case ACCESS_CAN_CONTROL_PACE:
            pb_bool = (vlc_bool_t*)va_arg( args, vlc_bool_t* );
            *pb_bool = VLC_TRUE;    /* FIXME */
            break;

        /* */
        case ACCESS_GET_MTU:
            pi_int = (int*)va_arg( args, int * );
            *pi_int = 0;
            break;
        case ACCESS_GET_SIZE:
            pi_64 = (int64_t*)va_arg( args, int64_t * );
            *pi_64 = p_sys->i_size > 0 ? p_sys->i_size : 0;
            break;
        case ACCESS_GET_POS:
            pi_64 = (int64_t*)va_arg( args, int64_t * );
            *pi_64 = p_sys->i_tell;
            break;
        case ACCESS_GET_EOF:
            pb_bool = (vlc_bool_t*)va_arg( args, vlc_bool_t* );
            *pb_bool = p_sys->b_eof;
            break;

        case ACCESS_GET_PTS_DELAY:
            pi_64 = (int64_t*)va_arg( args, int64_t * );
            var_Get( p_access, "http-caching", &val );
            *pi_64 = val.i_int * 1000;
            break;

        /* */
        case ACCESS_SET_PAUSE_STATE:
            /* Nothing to do */
            break;

        default:
            msg_Err( p_access, "unimplemented query in control" );
            return VLC_EGENERIC;

    }
    return VLC_SUCCESS;
}

/*****************************************************************************
 * ParseURL: extract user:password
 *****************************************************************************/
static void ParseURL( access_sys_t *p_sys, char *psz_url )
{
    char *psz_dup = strdup( psz_url );
    char *p = psz_dup;
    char *psz;

    /* Syntax //[user:password]@<hostname>[:<port>][/<path>] */
    while( *p == '/' )
    {
        p++;
    }
    psz = p;

    /* Parse auth */
    if( ( p = strchr( psz, '@' ) ) )
    {
        char *comma;

        *p++ = '\0';
        comma = strchr( psz, ':' );

        /* Retreive user:password */
        if( comma )
        {
            *comma++ = '\0';

            p_sys->psz_user = strdup( psz );
            p_sys->psz_passwd = strdup( comma );
        }
        else
        {
            p_sys->psz_user = strdup( psz );
        }
    }
    else
    {
        p = psz;
    }

    /* Parse uri */
    vlc_UrlParse( &p_sys->url, p, 0 );

    free( psz_dup );
}

/*****************************************************************************
 * Connect:
 *****************************************************************************/
static int Connect( access_t *p_access, int64_t i_tell )
{
    access_sys_t   *p_sys = p_access->p_sys;
    vlc_url_t      srv = p_sys->b_proxy ? p_sys->proxy : p_sys->url;
    char           *psz;

    /* Clean info */
    if( p_sys->psz_location ) free( p_sys->psz_location );
    if( p_sys->psz_mime ) free( p_sys->psz_mime );

    p_sys->psz_location = NULL;
    p_sys->psz_mime = NULL;
    p_sys->b_chunked = VLC_FALSE;
    p_sys->i_chunk = 0;
    p_sys->i_size = -1;
    p_sys->i_tell = i_tell;


    /* Open connection */
    p_sys->fd = net_OpenTCP( p_access, srv.psz_host, srv.i_port );
    if( p_sys->fd < 0 )
    {
        msg_Err( p_access, "cannot connect to %s:%d", srv.psz_host, srv.i_port );
        return VLC_EGENERIC;
    }

    if( p_sys->b_proxy )
    {
        net_Printf( VLC_OBJECT(p_access), p_sys->fd,
                    "GET http://%s:%d/%s HTTP/1.%d\r\n",
                    p_sys->url.psz_host, p_sys->url.i_port,
                    p_sys->url.psz_path, p_sys->i_version );
    }
    else
    {
        char *psz_path = p_sys->url.psz_path;
        if( !psz_path || !*psz_path )
        {
            psz_path = "/";
        }
        net_Printf( VLC_OBJECT(p_access), p_sys->fd,
                    "GET %s HTTP/1.%d\r\nHost: %s\r\n",
                    psz_path, p_sys->i_version, p_sys->url.psz_host );
    }
    /* User Agent */
    net_Printf( VLC_OBJECT(p_access), p_sys->fd, "User-Agent: %s\r\n",
                p_sys->psz_user_agent );
    /* Offset */
    if( p_sys->i_version == 1 )
    {
        net_Printf( VLC_OBJECT(p_access), p_sys->fd,
                    "Range: bytes="I64Fd"-\r\n", i_tell );
    }
    /* Authentification */
    if( p_sys->psz_user && *p_sys->psz_user )
    {
        char *buf;
        char *b64;

        asprintf( &buf, "%s:%s", p_sys->psz_user,
                   p_sys->psz_passwd ? p_sys->psz_passwd : "" );

        b64 = b64_encode( buf );

        net_Printf( VLC_OBJECT(p_access), p_sys->fd,
                    "Authorization: Basic %s\r\n", b64 );
        free( b64 );
    }
    net_Printf( VLC_OBJECT(p_access), p_sys->fd, "Connection: Close\r\n" );

    if( net_Printf( VLC_OBJECT(p_access), p_sys->fd, "\r\n" ) < 0 )
    {
        msg_Err( p_access, "failed to send request" );
        net_Close( p_sys->fd ); p_sys->fd = -1;
        return VLC_EGENERIC;
    }

    /* Read Answer */
    if( ( psz = net_Gets( VLC_OBJECT(p_access), p_sys->fd ) ) == NULL )
    {
        msg_Err( p_access, "failed to read answer" );
        goto error;
    }
    if( !strncmp( psz, "HTTP/1.", 7 ) )
    {
        p_sys->psz_protocol = "HTTP";
        p_sys->i_code = atoi( &psz[9] );
    }
    else if( !strncmp( psz, "ICY", 3 ) )
    {
        p_sys->psz_protocol = "ICY";
        p_sys->i_code = atoi( &psz[4] );
    }
    else
    {
        msg_Err( p_access, "invalid HTTP reply '%s'", psz );
        free( psz );
        goto error;
    }
    msg_Dbg( p_access, "protocol '%s' answer code %d",
             p_sys->psz_protocol, p_sys->i_code );
    if( !strcmp( p_sys->psz_protocol, "ICY" ) )
    {
        p_sys->b_seekable = VLC_FALSE;
    }
    if( p_sys->i_code != 206 )
    {
        p_sys->b_seekable = VLC_FALSE;
    }
    if( p_sys->i_code >= 400 )
    {
        msg_Err( p_access, "error: %s", psz );
        free( psz );
        goto error;
    }
    free( psz );

    for( ;; )
    {
        char *psz = net_Gets( VLC_OBJECT(p_access), p_sys->fd );
        char *p;

        if( psz == NULL )
        {
            msg_Err( p_access, "failed to read answer" );
            goto error;
        }

        /* msg_Dbg( p_input, "Line=%s", psz ); */
        if( *psz == '\0' )
        {
            free( psz );
            break;
        }


        if( ( p = strchr( psz, ':' ) ) == NULL )
        {
            msg_Err( p_access, "malformed header line: %s", psz );
            free( psz );
            goto error;
        }
        *p++ = '\0';
        while( *p == ' ' ) p++;

        if( !strcasecmp( psz, "Content-Length" ) )
        {
            p_sys->i_size = i_tell + atoll( p );
            msg_Dbg( p_access, "stream size="I64Fd, p_sys->i_size );
        }
        else if( !strcasecmp( psz, "Location" ) )
        {
            if( p_sys->psz_location ) free( p_sys->psz_location );
            p_sys->psz_location = strdup( p );
        }
        else if( !strcasecmp( psz, "Content-Type" ) )
        {
            if( p_sys->psz_mime ) free( p_sys->psz_mime );
            p_sys->psz_mime = strdup( p );
            msg_Dbg( p_access, "Content-Type: %s", p_sys->psz_mime );
        }
        else if( !strcasecmp( psz, "Transfer-Encoding" ) )
        {
            msg_Dbg( p_access, "Transfer-Encoding: %s", p );
            if( !strncasecmp( p, "chunked", 7 ) )
            {
                p_sys->b_chunked = VLC_TRUE;
            }
        }

        free( psz );
    }
    return VLC_SUCCESS;

error:
    net_Close( p_sys->fd ); p_sys->fd = -1;
    return VLC_EGENERIC;
}

/*****************************************************************************
 * b64_encode:
 *****************************************************************************/
static char *b64_encode( unsigned char *src )
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    char *dst = malloc( strlen( src ) * 4 / 3 + 12 );
    char *ret = dst;
    unsigned i_bits = 0;
    unsigned i_shift = 0;

    for( ;; )
    {
        if( *src )
        {
            i_bits = ( i_bits << 8 )|( *src++ );
            i_shift += 8;
        }
        else if( i_shift > 0 )
        {
           i_bits <<= 6 - i_shift;
           i_shift = 6;
        }
        else
        {
            *dst++ = '=';
            break;
        }

        while( i_shift >= 6 )
        {
            i_shift -= 6;
            *dst++ = b64[(i_bits >> i_shift)&0x3f];
        }
    }

    *dst++ = '\0';

    return ret;
}
