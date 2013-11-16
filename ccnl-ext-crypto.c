/*
 * @f ccnl-ext-crypto.c
 * @b CCN lite extension, crypto logic (sign, verify, encrypt, decrypt)
 *
 * Copyright (C) 2012-13, Christian Tschudin, University of Basel
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * File history:
 * 2012-10-03 created
 */
#ifndef CCNL_EXT_CRYPTO_C
#define CCNL_EXT_CRYPTO_C

#ifdef CCNL_USE_MGMT_SIGNATUES
#include "ccnl-core.h"
#include "ccnl-ext-debug.c"
#include "ccnx.h"
#include "ccnl-includes.h"
#include "ccnl.h"
#include "ccnl-ext-mgmt.c"
#include "ccnl-pdu.c"

char buf[64000];
int plen;
int received;

static int 
ccnl_crypto_strtoint(char *str){
#ifdef CCNL_LINUXKERNEL
    return strtol(str,NULL,0);
#else
    return strtol(str,NULL,0);
#endif
}


static int 
ccnl_mgmt_handle(struct ccnl_relay_s *ccnl, struct ccnl_buf_s *orig,
	  struct ccnl_prefix_s *prefix, struct ccnl_face_s *from, 
        char *cmd, int verified);

#ifndef CCNL_LINUXKERNEL
static int
ccnl_crypto_ux_open(char *frompath)
{
    int sock, bufsize;
    struct sockaddr_un name;

    /* Create socket for sending */
    sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0) {
	perror("opening datagram socket");
	exit(1);
    }
    unlink(frompath);
    name.sun_family = AF_UNIX;
    strcpy(name.sun_path, frompath);
    if (bind(sock, (struct sockaddr *) &name,
	     sizeof(struct sockaddr_un))) {
	perror("binding name to datagram socket");
	exit(1);
    }
//    printf("socket -->%s\n", NAME);

    bufsize = 4 * CCNL_MAX_PACKET_SIZE;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

    return sock;
}
#else
static struct socket*
ccnl_crypto_ux_open(char *path, struct sockaddr_un *ux)
{
    struct socket *s;
    int rc;

    DEBUGMSG(99, "ccnl_open_unixpath %s\n", path);
    //unlink(path);
    rc = sock_create(AF_UNIX, SOCK_DGRAM, 0, &s);
    if (rc < 0) {
	DEBUGMSG(1, "Error %d creating UNIX socket %s\n", rc, path);
	return NULL;
    }
    DEBUGMSG(9, "UNIX socket is %p\n", (void*)s);
   
    ux->sun_family = AF_UNIX;
    strcpy(ux->sun_path, path);
    rc = s->ops->bind(s, (struct sockaddr*) ux,
		offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1);
    if (rc < 0) {
	DEBUGMSG(1, "Error %d binding UNIX socket to %s "
		    "(remove first, check access rights)\n", rc, path);
	goto Bail;
    }

    return s;

Bail:
    sock_release(s);
    return NULL;
}
#endif

static int 
ccnl_crypto_get_tag_content(unsigned char **buf, int *len, char *content, int contentlen){
    int num = 0;
    memset(content,0,contentlen);
    while((**buf) !=  0 && num < contentlen)
    {
        content[num] = **buf;
        ++(*buf); --(*len);
        ++num;
    }
    ++(*buf); --(*len);
    return num;
}

#define extractStr2(VAR,DTAG) \
    if (typ == CCN_TT_DTAG && num == DTAG) { \
	char *s; unsigned char *valptr; int vallen; \
	if (consume(typ, num, buf, buflen, &valptr, &vallen) < 0) goto Bail; \
	s = ccnl_malloc(vallen+1); if (!s) goto Bail; \
	memcpy(s, valptr, vallen); s[vallen] = '\0'; \
	ccnl_free(VAR); \
	VAR = (unsigned char*) s; \
	continue; \
    } do {} while(0)

static int 
ccnl_crypto_create_ccnl_crypto_face(struct ccnl_relay_s *relay, char *ux_path)
{
    sockunion su;
    DEBUGMSG(99, "  adding UNIX face unixsrc=%s\n", ux_path);
    su.sa.sa_family = AF_UNIX;
    strcpy(su.ux.sun_path, (char*) ux_path);
    relay->crypto_face = ccnl_get_face_or_create(relay, -1, &su.sa, sizeof(struct sockaddr_un));
    if(!relay->crypto_face) return 0;
    relay->crypto_face->flags = CCNL_FACE_FLAGS_STATIC;
    
    return 1;
}

static int 
ccnl_crypto_create_ccnl_sign_verify_msg(char *typ, int txid, char *content, int content_len,
        char *sig, int sig_len, char *msg, char *callback)
{
    int len = 0, len2 = 0, len3 = 0;
    char *component_buf, *contentobj_buf;
    char h[100];
    
    component_buf = ccnl_malloc(sizeof(char)*(content_len)+2000);
    contentobj_buf = ccnl_malloc(sizeof(char)*(content_len)+1000);
    
    len = mkHeader(msg, CCN_DTAG_INTEREST, CCN_TT_DTAG);   // interest
    len += mkHeader(msg+len, CCN_DTAG_NAME, CCN_TT_DTAG);  // name

    len += mkStrBlob(msg+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "ccnx");
    len += mkStrBlob(msg+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG, "crypto");
    len += mkStrBlob(msg+len, CCNL_DTAG_CALLBACK, CCN_TT_DTAG, callback);
    
    // prepare FACEINSTANCE
    
    memset(h, 0, 100);
    sprintf(h, "%d", txid);
    len3 += mkStrBlob(component_buf+len3, CCN_DTAG_SEQNO, CCN_TT_DTAG, h);
    if(!strcmp(typ, "verify"))
        len3 += mkBlob(component_buf+len3, CCN_DTAG_SIGNATURE, CCN_TT_DTAG,  // content
		   (char*) sig, sig_len);
    len3 += mkBlob(component_buf+len3, CCN_DTAG_CONTENTDIGEST, CCN_TT_DTAG,  // content
		   (char*) content, content_len);
    
    // prepare CONTENTOBJ with CONTENT
    len2 = mkHeader(contentobj_buf, CCN_DTAG_CONTENTOBJ, CCN_TT_DTAG);   // contentobj
    len2 += mkStrBlob(contentobj_buf+len2, CCN_DTAG_TYPE, CCN_TT_DTAG, typ);
    len2 += mkBlob(contentobj_buf+len2, CCN_DTAG_CONTENT, CCN_TT_DTAG,  // content
		   (char*) component_buf, len3);
    contentobj_buf[len2++] = 0; // end-of-contentobj

    // add CONTENTOBJ as the final name component
    len += mkBlob(msg+len, CCN_DTAG_COMPONENT, CCN_TT_DTAG,  // comp
		  (char*) contentobj_buf, len2);

    msg[len++] = 0; // end-of-name
    msg[len++] = 0; // end-o
    
    ccnl_free(component_buf);
    ccnl_free(contentobj_buf);
    
    return len;
}

static int
ccnl_crypto_extract_type_callback(unsigned char **buf, int *buflen, char *type, 
        int max_type_length, char* callback, int max_callback_length)
{
    int typ, num;
    char comp1[10];
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_CONTENTOBJ) goto Bail;
    
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_NAME) goto Bail;
    
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_COMPONENT) goto Bail;
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail; 
    ccnl_crypto_get_tag_content(buf,buflen, comp1, sizeof(comp1));
    
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_COMPONENT) goto Bail;
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail; 
    ccnl_crypto_get_tag_content(buf,buflen, comp1, sizeof(comp1));
    
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCNL_DTAG_CALLBACK) goto Bail;
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail; 
    ccnl_crypto_get_tag_content(buf,buflen, callback, max_callback_length);

    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_COMPONENT) goto Bail;
    
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail; 
    
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_CONTENTOBJ) goto Bail;
    
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_TYPE) goto Bail;
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail;
    ccnl_crypto_get_tag_content(buf,buflen, type, max_type_length);
    return 1;
    Bail:
    return 0;
}

static int
ccnl_crypto_extract_msg(unsigned char **buf, int *buflen, char **msg){
    
    int len = 0;
    int num, typ;    
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_CONTENT) goto Bail;
    
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail;

    *msg = *buf;
    len = (*buflen) - 6;
    
    return len;
    Bail:
    DEBUGMSG(99, "Failed to extract msg\n");
    return 0;
}

static int
ccnl_crypto_get_signature(unsigned char **buf, int *buflen, char *sig, int sig_len)
{
    int num = 0;
    while(sig_len > 0){
        sig[num] = **buf;
        ++num;
        --sig_len;
        ++(*buf);
        --(*buflen);
    }
    return 1;
}

static int 
ccnl_crypto_extract_sign_reply(unsigned char **buf, int *buflen, char *sig, int *sig_len, int *seqnum)
{
    int ret = 0;
    char type[100];
    int num, typ;
    char seqnumber_s[100];
    char siglen_s[100];
    int seqnubmer;
    int siglen = 0;
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_CONTENT) goto Bail;
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail;
    
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_SEQNO) goto Bail;
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail;
    ccnl_crypto_get_tag_content(buf, buflen, seqnumber_s, sizeof(seqnumber_s));
    seqnubmer = ccnl_crypto_strtoint(seqnumber_s);
    *seqnum = seqnubmer;
    
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_SIGNEDINFO) goto Bail;
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail;
    ccnl_crypto_get_tag_content(buf, buflen, siglen_s, sizeof(siglen_s));
    siglen = ccnl_crypto_strtoint(siglen_s);
    *sig_len = siglen;
    
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_SIGNATURE) goto Bail;
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail;
    ccnl_crypto_get_signature(buf, buflen, sig, siglen);
    ++(*buf);
    --(*buflen);
      
    *sig_len = siglen;
    ret = 1;
    Bail:
    return ret;
}

static int 
ccnl_crypto_extract_verify_reply(unsigned char **buf, int *buflen, int *seqnum)
{
    int verified = 0;
    char type[100];
    int num, typ;
    char seqnumber_s[100], verified_s[100];
    int seqnubmer, h;
    
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_CONTENT) goto Bail;
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail;
    
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCN_DTAG_SEQNO) goto Bail;
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail;
    ccnl_crypto_get_tag_content(buf, buflen, seqnumber_s, sizeof(seqnumber_s));
    seqnubmer = ccnl_crypto_strtoint(seqnumber_s);
    *seqnum = seqnubmer;
    
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_DTAG || num != CCNL_DTAG_VERIFIED) goto Bail;
    if(dehead(buf, buflen, &num, &typ)) goto Bail;
    if (typ != CCN_TT_BLOB) goto Bail;
    ccnl_crypto_get_tag_content(buf, buflen, verified_s, sizeof(verified_s));
    h = ccnl_crypto_strtoint(verified_s);
    if(h == 1) verified = 1;
    
    Bail:
    return verified;
}

static int 
ccnl_crypto_add_signature(unsigned char *out, char *sig, int siglen)
{
    int len;

    len = mkHeader(out, CCN_DTAG_SIGNATURE, CCN_TT_DTAG);
    len += mkStrBlob(out + len, CCN_DTAG_NAME, CCN_TT_DTAG, "SHA256");
    len += mkStrBlob(out + len, CCN_DTAG_WITNESS, CCN_TT_DTAG, "");
        
    //add signaturebits bits...
    len += mkHeader(out + len, CCN_DTAG_SIGNATUREBITS, CCN_TT_DTAG);
    len += addBlob(out + len, sig, siglen);
    out[len++] = 0; // end signaturebits
    
    out[len++] = 0; // end signature
    return len;
}

/**
 * 
 * @param ccnl
 * @param content
 * @param content_len
 * @param sig
 * @param sig_len
 * @param callback function which should be called when crypto system returns
 *              for a new callback function you have to extend ccnl_crypto()!!!!
 * @return 
 */
int 
ccnl_crypto_sign(struct ccnl_relay_s *ccnl, char *content, int content_len, 
        char *callback, int seqnum)
{
    
    //char *buf = 0;
    char *msg = 0; int len;
    struct ccnl_buf_s *retbuf;
    int ret = 0; //, plen = 0;
    plen = 0;
    memset(buf,0,sizeof(buf));
    //create ccn_msg
    if(!ccnl->crypto_face) return 0;
    msg = (char *) ccnl_malloc(sizeof(char)*(content_len)+3000);
    
    len = ccnl_crypto_create_ccnl_sign_verify_msg("sign", seqnum, content, content_len, 
            NULL, NULL, msg, callback);
    
    //send ccn_msg to crytoserver
    retbuf = ccnl_buf_new((char *)msg, len);
    
    ccnl_face_enqueue(ccnl, ccnl->crypto_face, retbuf);
    
    Bail:
    if(msg) ccnl_free(msg);
    return ret;   
}

/**
 * 
 * @param ccnl
 * @param content
 * @param content_len
 * @param sig
 * @param sig_len
 * @param callback function which should be called when crypto system returns
 *              for a new callback function you have to extend ccnl_crypto()!!!!
 * @return 
 */ 
int 
ccnl_crypto_verify(struct ccnl_relay_s *ccnl, char *content, int content_len,
        char *sig, int sig_len, char* callback, int sequnum)
{
    char *msg = 0;
    int len = 0, ret = 0;
    struct ccnl_buf_s *retbuf;
    //int plen;
    //unsigned char *buf;
    plen = 0;
    memset(buf,0,sizeof(buf));
    if(!ccnl->crypto_face) return ret;
    
    msg = (char *)ccnl_malloc(sizeof(char)*(content_len+sig_len)+3000);
    
    len = ccnl_crypto_create_ccnl_sign_verify_msg("verify", sequnum, content, 
            content_len, sig, sig_len, msg, callback);

    //send ccn_msg to crytoserver
    retbuf = ccnl_buf_new((char *)msg, len);
    ccnl_face_enqueue(ccnl, ccnl->crypto_face, retbuf);
    
    if(msg) ccnl_free(msg); 
    return ret;
}

int
ccnl_mgmt_crypto(struct ccnl_relay_s *ccnl, char *type, char *buf, int buflen)
{
    
   struct ccnl_face_s *from;
   DEBUGMSG(99,"ccnl_crypto type: %s\n", type);  
  
   if(!strcmp(type, "verify")){
      int seqnum;
      int verified = ccnl_crypto_extract_verify_reply(&buf, &buflen, &seqnum);
      char *msg, *msg2; 
      char cmd[500];
      int len = ccnl_crypto_extract_msg(&buf, &buflen, &msg), len2 = 0;
      struct ccnl_face_s *from;
      //DEBUGMSG(99,"VERIFIED: %d, MSG_LEN: %d\n", verified, len);
      
      int rc= -1, scope=3, aok=3, minsfx=0, maxsfx=CCNL_MAX_NAME_COMP, contlen;
      struct ccnl_buf_s *buf1 = 0, *nonce=0, *ppkd=0;
      struct ccnl_interest_s *i = 0;
      struct ccnl_content_s *c = 0;
      struct ccnl_prefix_s *p = 0;
      unsigned char *content = 0;
      
      msg2 = (char *) ccnl_malloc(sizeof(char) * len + 200);
      len2 = mkHeader(msg2,CCN_DTAG_NAME, CCN_TT_DTAG);
      memcpy(msg2+len2, msg, len);
      len2 +=len;
      msg2[len2++] = 0;
      
      from = ccnl->faces;
      while(from){
          if(from->faceid == seqnum) 
              break;
          from = from->next;
      }
      
      buf1 = ccnl_extract_prefix_nonce_ppkd(&msg2, &len2, &scope, &aok, &minsfx,
			 &maxsfx, &p, &nonce, &ppkd, &content, &contlen);

      if (p->complen[2] < sizeof(cmd)) {
            memcpy(cmd, p->comp[2], p->complen[2]);
            cmd[p->complen[2]] = '\0';
      } else
            strcpy(cmd, "cmd-is-too-long-to-display");
      ccnl_mgmt_handle(ccnl, msg2, p, from, cmd, verified);
   }else if(!strcmp(type, "sign")){
      char *sig = (char *) ccnl_malloc(sizeof(char)* CCNL_MAX_PACKET_SIZE);
      char *out;
      char *msg;
      int siglen, seqnum, len, len1;
      struct ccnl_buf_s *retbuf;
      ccnl_crypto_extract_sign_reply(&buf, &buflen, sig, &siglen, &seqnum);
      
      len = ccnl_crypto_extract_msg(&buf, &buflen, &msg);
      out = (char *) ccnl_malloc(sizeof(char)*len + sizeof(char)*siglen + 4096);
      
      len1 = mkHeader(out, CCN_DTAG_CONTENTOBJ, CCN_TT_DTAG);   // content
      len1 += mkHeader(out+len1, CCN_DTAG_NAME, CCN_TT_DTAG);  // name
      if(siglen > 0) len1 += ccnl_crypto_add_signature(out+len1, sig, siglen);
      
      memcpy(out+len1, msg, len);
      len1 +=len;
      
      out[len1++] = 0; // end-of-name
      out[len1++] = 0; // end-of-interest
      DEBUGMSG(99, "LEN1: %d", len1);
      from = ccnl->faces;
      while(from){
          if(from->faceid == seqnum) 
              break;
          from = from->next;
      }
      
      retbuf = ccnl_buf_new((char *)out, len1);
      if(seqnum >= 0){
          ccnl_face_enqueue(ccnl, from, retbuf); 
      }else{
          struct ccnl_prefix_s *prefix_a = 0;
          struct ccnl_content_s *c = 0;
          struct ccnl_buf_s *nonce=0, *ppkd=0, *pkt = 0;
          unsigned char *content = 0;
          char *ht = (char *) ccnl_malloc(sizeof(char)*20);
          int contlen;
          pkt = ccnl_extract_prefix_nonce_ppkd(&out, &len1, 0, 0,
                         0, 0, &prefix_a, &nonce, &ppkd, &content, &contlen);
          
          if (!pkt) {
               DEBUGMSG(6, " parsing error\n"); goto Done;
          }
          if (prefix_a) {
              //DEBUGMSG(99, "%s", prefix_a->comp);
              //ccnl_free(prefix_a);
          }
          //prefix_a = (struct ccnl_prefix_s *)ccnl_malloc(sizeof(struct ccnl_prefix_s));
          prefix_a->compcnt = 2;
          prefix_a->comp = (char **) ccnl_malloc(sizeof(char*)*2);
          prefix_a->comp[0] = "debug";
          sprintf(ht, "seqnum-%d", -seqnum);
          prefix_a->comp[1] = ht;
          *prefix_a->complen = (int *) ccnl_malloc(sizeof(int)*2);
          prefix_a->complen[0] = strlen("debug");
          prefix_a->complen[1] = strlen(ht);
          c = ccnl_content_new(ccnl, &pkt, &prefix_a, &ppkd,
                                content, contlen);
          if (!c) goto Done;
          
          ccnl_content_serve_pending(ccnl, c);
          ccnl_content_add2cache(ccnl, c);
      }
      Done:
      ccnl_free(out);      
   }
}

int
ccnl_crypto(struct ccnl_relay_s *ccnl, struct ccnl_buf_s *orig,
	  struct ccnl_prefix_s *prefix, struct ccnl_face_s *from)
{
      
    char *buf = orig->data;
    int buflen = orig->datalen;
    char type[100];
    char callback[100];
    
    
    if(!ccnl_crypto_extract_type_callback(&buf, &buflen, type, sizeof(type), callback, 
    sizeof(callback))) goto Bail;
    
    DEBUGMSG(99,"Callback: %s Type: %s\n", callback, type); 
    
    if(!strcmp(callback, "ccnl_mgmt_crypto")) 
        ccnl_mgmt_crypto(ccnl, type, buf, buflen);
    /**
     * Add here further callback functions
     * else if(!strcmp(callback, "")){
     *   
     *}
     */
    
    Bail: 
    return -1;
}

#endif /*CCNL_USE_MGMT_SIGNATUES*/
#endif /*CCNL_EXT_CRYPTO_C*/