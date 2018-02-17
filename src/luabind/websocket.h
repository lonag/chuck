#define WEBSOCKET_METATABLE "lua_websocket"

#define WEBSOCKET_FRAME_HEAD_METATABLE "lua_websocket_frame_head"

enum {
    TEXT_FRAME   = 0x01,
    BINARY_FRAME = 0x02,
    CLOSE_FRAME  = 0x08,
    PING_FRAME   = 0x09,
    PONG_FRAME   = 0x0A
};

typedef struct {
	union {
		struct {
			uint8_t FIN :1;
			uint8_t RSV1:1;
			uint8_t RSV2:1;
			uint8_t RSV3:1;
			uint8_t opcode:4;
		};
		uint8_t data;
	};
} byte1;

typedef struct {
	union {
		struct {
			uint8_t mask :1;
			uint8_t payload :7;
		};
		uint8_t data;
	};
} byte2;

enum {
	WEBSOCKET_SERVER = 1,
	WEBSOCKET_CLIENT = 2,
};

typedef struct {
	chk_stream_socket    *socket;
	chk_luaRef            cb;
	int8_t                type;
} websocket;

#define lua_check_websocket(L,I)	\
	(websocket*)luaL_checkudata(L,I,WEBSOCKET_METATABLE)

#define lua_check_websocket_fream_head(L,I) \
	(byte1*)luaL_checkudata(L,I,WEBSOCKET_FRAME_HEAD_METATABLE)

typedef struct websocket_decoder {
	void (*update)(chk_decoder*,chk_bytechunk *b,uint32_t spos,uint32_t size);
	chk_bytebuffer *(*unpack)(chk_decoder*,int32_t *err);
	void (*release)(chk_decoder*);
	uint32_t       spos;
	uint32_t       size;
	chk_bytechunk  *b;
	chk_bytebuffer *frame;
}websocket_decoder;

static void websocket_update(chk_decoder *_,chk_bytechunk *c,uint32_t spos,uint32_t size) {
	websocket_decoder *d = ((websocket_decoder*)_);
    if(!d->b) {
	    d->b = chk_bytechunk_retain(c);
	    d->spos  = spos;
	    d->size = 0;
    }
    d->size += size;
}

static chk_bytebuffer *websocket_unpack(chk_decoder *_,int32_t *err) {
	websocket_decoder *d = ((websocket_decoder*)_);
	chk_bytebuffer *frame;
	int32_t errcode = 0;
	int32_t fin = 0;
	uint32_t offset = 0;
	if(d->size >= 2) {

		chk_bytebuffer buffer;
		chk_bytebuffer_init(&buffer,d->b,d->spos,d->size,READ_ONLY);
		do{
			byte1 b1;
			byte2 b2;
			uint8_t mask[4];

			if(sizeof(b1) != chk_bytebuffer_read(&buffer,offset,(char*)&b1,1)) {
				errcode = chk_error_unpack;
				break;
			}
			offset += 1; 

			if(sizeof(b2) != chk_bytebuffer_read(&buffer,offset,(char*)&b2,1)) {
				errcode = chk_error_unpack;
				break;
			}
			offset += 1;

			if((b1.opcode == CLOSE_FRAME || b1.opcode == PING_FRAME || b1.opcode == PONG_FRAME) && b1.FIN == 0) {
				errcode = chk_error_unpack;
				break;
			}

			fin = b1.FIN;

			uint64_t payload;
			//读取payload大小

			if(b2.payload <= 125) {
				payload = b2.payload;
			} else if(b2.payload == 126) {
				if(d->size < 4) { //2 + 2
					break;
				}
				uint16_t payload16;
				if(sizeof(payload16) != chk_bytebuffer_read(&buffer,offset,(char*)&payload16,sizeof(payload16))) {
					errcode = chk_error_unpack;
					break;
				}
				payload = chk_ntoh16(payload16);				 
				offset += 2;
			} else { //127
				if(d->size < 10) {//2 + 8
					break;
				}
				uint64_t payload64;
				if(sizeof(payload64) != chk_bytebuffer_read(&buffer,offset,(char*)&payload64,sizeof(payload64))) {
					errcode = chk_error_unpack;
					break;
				}
				payload = chk_ntoh64(payload64);				 
				offset += 8;				
			}

			if(payload >= 0xFFFFFFFF) {
				//只支持整个完整数据包payload小于u32
				errcode = chk_error_unpack;
				break;
			}

			if(1 == b2.mask) {
				if(d->size < offset + sizeof(mask) + payload) {
					break;
				}
				if(sizeof(mask) != chk_bytebuffer_read(&buffer,offset,(char*)mask,sizeof(mask))) {
					errcode = chk_error_unpack;
					break;
				}
				offset += sizeof(mask);				
			} else {
				if(d->size < offset + payload) {
					break;
				}
			}

			if(NULL == d->frame) {
				d->frame = chk_bytebuffer_new(1 + payload);
				if(d->frame == NULL) {
					errcode = chk_error_no_memory;
					break;
				}
				chk_bytebuffer_append_byte(d->frame,*(uint8_t*)&b1);
			}

			uint64_t i;
			uint8_t  byte;
			for(i = 0; i < payload; ++i,++offset) {
				chk_bytebuffer_read(&buffer,offset,(char*)&byte,sizeof(byte));
				byte = byte ^ mask[i%4];
				if(chk_error_ok != (errcode = chk_bytebuffer_append_byte(d->frame,byte))) {
					break;
				}
			}
			if(errcode != chk_error_ok) {
				break;
			}

		}while(0);
		
		chk_bytebuffer_finalize(&buffer);
	}

	if(err) {
		*err = errcode;
	}

	if(0 == errcode && fin) {
		frame = d->frame;
		d->frame = NULL;

		chk_bytechunk *head;
		uint32_t size;

		//调整pos及其b
		do {
			head = d->b;
			size = head->cap - d->spos;
			size = offset > size ? size:offset;
			d->spos += size;
			offset  -= size;
			d->size -= size;
			if(d->spos >= head->cap) { //当前b数据已经用完
				d->b = chk_bytechunk_retain(head->next);
				chk_bytechunk_release(head);
				d->spos = 0;
				if(!d->b) break;
			}
		}while(offset);

		return frame;
	} else {
		return NULL;
	}
}

static void release_decoder(chk_decoder *_) {
	websocket_decoder *d = ((websocket_decoder*)_);
	if(d->b) chk_bytechunk_release(d->b);
	if(d->frame) chk_bytebuffer_del(d->frame);
	free(d);
}

static websocket_decoder *new_websocket_decoder() {
	websocket_decoder *d = calloc(1,sizeof(*d));
	if(!d){
		CHK_SYSLOG(LOG_ERROR,"calloc websocket_decoder failed");		 
		return NULL;
	}
	d->update   = websocket_update;
	d->unpack   = websocket_unpack;
	d->release  = release_decoder;
	return d;
}	

void chk_stream_socket_set_decoder(chk_stream_socket *s,chk_decoder *decoder);

static int upgrade(uint8_t type,lua_State *L) {
	http_connection   *conn = lua_check_http_connection(L,1);
	if(conn->socket && conn->parser) {
		websocket *ws = LUA_NEWUSERDATA(L,websocket);
		if(!ws) {
			CHK_SYSLOG(LOG_ERROR,"LUA_NEWUSERDATA() failed");			
			return 0;
		}

		ws->type = type;

		websocket_decoder *decoder = new_websocket_decoder();
		if(NULL == decoder) {
			CHK_SYSLOG(LOG_ERROR,"new_websocket_decoder() failed");			
			return 0;
		}
		ws->socket = conn->socket;
		release_lua_http_parser(conn->parser);
		chk_luaRef_release(&conn->cb);
		conn->socket = NULL;
		conn->parser = NULL;
		//从event_loop移除，待用户调用start后重新绑定
		chk_loop_remove_handle((chk_handle*)ws->socket);
		//将decoder替换成websocket_decoder
		chk_stream_socket_set_decoder(ws->socket,(chk_decoder*)decoder);
		//http_connection的parser是通过close_callback释放的，这里已经提前释放所以需要把close_callback设置为空
		chk_stream_socket_set_close_callback(ws->socket,NULL,chk_ud_make_void(NULL));
		chk_stream_socket_setUd(ws->socket,chk_ud_make_void(ws));
		luaL_getmetatable(L, WEBSOCKET_METATABLE);
		lua_setmetatable(L, -2);
		return 1;
	} else {
		return 0;
	}	
}

static int lua_server_upgrade(lua_State *L) {
	return upgrade(WEBSOCKET_SERVER,L);
}

static int lua_client_upgrade(lua_State *L) {
	return upgrade(WEBSOCKET_CLIENT,L);	
}

static int lua_websocket_gc(lua_State *L) {
	websocket *ws = lua_check_websocket(L,1);
	if(ws->socket) {
		chk_stream_socket_setUd(ws->socket,chk_ud_make_void(NULL));
		chk_stream_socket_close(ws->socket,0);
	}
	if(ws->cb.L) {
		chk_luaRef_release(&ws->cb);
	}	
	return 0;
}

static chk_bytebuffer *build_ws_frame(int wsType,byte1 head,chk_bytebuffer *data) {
	size_t buffSize = 2;
	byte2 b2;
	chk_bytebuffer *buff = NULL;
	if(data) {
		b2.mask = (wsType == WEBSOCKET_CLIENT ? 1:0);
		if(b2.mask == 1) {
			buffSize += 4;
		}
		if(data->datasize < 126) {
			b2.payload = data->datasize;
		} else if(data->datasize <= 0xFFFF) {
			b2.payload = 126;
			buffSize += 2;
		} else {
			b2.payload = 127;
			buffSize += 8;			
		}
	} else {
		b2.mask = 0;
		b2.payload = 0;
	}

	buff = chk_bytebuffer_new(buffSize);
	if(!buff) {
		return NULL;
	}
	chk_bytebuffer_append_byte(buff,head.data);
	chk_bytebuffer_append_byte(buff,b2.data);
	if(b2.payload == 126) {
		chk_bytebuffer_append_word(buff,chk_hton16(data->datasize));
	} else if(b2.payload == 127) {
		chk_bytebuffer_append_qword(buff,chk_hton64(data->datasize));
	}
	uint32_t i;
	uint8_t  byte;
	if(b2.mask) {
		uint32_t mask = (uint32_t)rand();
		chk_bytebuffer_append_dword(buff,mask);
		for(i = 0; i < data->datasize; ++i) {
			chk_bytebuffer_read(data,i,(char*)&byte,1);
			byte = byte ^ ((uint8_t*)&mask)[i%4];
			chk_bytebuffer_append_byte(buff,byte);
		}
	} else {
		for(i = 0; i < data->datasize; ++i) {
			chk_bytebuffer_read(data,i,(char*)&byte,1);
			chk_bytebuffer_append_byte(buff,byte);
		}
	}

	return buff;
}

/*
static int lua_websocket_ping(lua_State *L) {
	chk_bytebuffer *appData = NULL;
	websocket *ws = lua_check_websocket(L,1);
	if(ws->socket) {
		if(lua_isuserdata(L,2)) {
			appData = lua_checkbytebuffer(L,2);
		}
		byte1 head;
		head.data = 0;
		head.opcode = PING_FRAME;
		chk_bytebuffer *buff = build_ws_frame(ws->type,head,appData);
		if(!buff) {
			return luaL_error(L,"no memory!");
		}
		chk_stream_socket_send(ws->socket,buff);		
	}
	return 0;
}

static int lua_websocket_pong(lua_State *L) {
	chk_bytebuffer *appData = NULL;
	websocket *ws = lua_check_websocket(L,1);
	if(ws->socket) {
		if(lua_isuserdata(L,2)) {
			appData = lua_checkbytebuffer(L,2);
		}
		byte1 head;
		head.data = 0;
		head.opcode = PONG_FRAME;
		chk_bytebuffer *buff = build_ws_frame(ws->type,head,appData);
		if(!buff) {
			return luaL_error(L,"no memory!");
		}
		chk_stream_socket_send(ws->socket,buff);		
	}
	return 0;
}
*/

static int lua_websocket_send(lua_State *L) {
	websocket *ws = lua_check_websocket(L,1);
	if(!ws->socket){
		lua_pushstring(L,"socket close");		
		return 1;
	}
	byte1 head;
	head.data = 0;
	const char *opcode = luaL_checkstring(L,2);
	if(strcmp("text",opcode) == 0) {
		head.opcode = TEXT_FRAME;
	} else if(strcmp("binary",opcode) == 0) {
		head.opcode = BINARY_FRAME;
	} else if(strcmp("ping",opcode) == 0) {
		head.opcode = PING_FRAME;
	} else if(strcmp("pong",opcode) == 0) {
		head.opcode = PONG_FRAME;
	} else {
		return luaL_error(L,"opcode must be binary|text|ping|pong");
	}

	chk_bytebuffer *data = lua_checkbytebuffer(L,3);
	chk_bytebuffer *buff = build_ws_frame(ws->type,head,data);
	if(!buff) {
		return luaL_error(L,"no memory!");
	}
	if(0 != chk_stream_socket_send(ws->socket,buff)){
		lua_pushstring(L,"send error");
		return 1;
	}
	return 0;
}

static int lua_websocket_close(lua_State *L) {
	websocket *ws = lua_check_websocket(L,1);
	if(ws->socket) {
		size_t s;
		const char *reason = lua_tolstring(L,2,&s);
		chk_bytebuffer *data = NULL;
		if(reason){
			data = chk_bytebuffer_new(s);
			if(!data) {
				return luaL_error(L,"no memory!");
			}
			chk_bytebuffer_append(data,(uint8_t*)reason,s);
		}

		byte1 head;
		head.data = 0;
		head.opcode = CLOSE_FRAME;
		chk_bytebuffer *buff = build_ws_frame(ws->type,head,data);
		chk_bytebuffer_del(data);
		if(!buff) {
			return luaL_error(L,"no memory!");
		}
		chk_stream_socket_send(ws->socket,buff);
		chk_stream_socket_setUd(ws->socket,chk_ud_make_void(NULL));
		chk_stream_socket_close(ws->socket,5);
		ws->socket = NULL;
		if(ws->cb.L) {
			chk_luaRef_release(&ws->cb);
		}
	}
	return 0;
}


static void ws_event_cb(chk_stream_socket *s,chk_bytebuffer *frame,int32_t error) {
	
	static const char *ping   = "ping";
	static const char *pong   = "pong";
	static const char *text   = "text";
	static const char *binary = "binary";

	websocket *ws = chk_stream_socket_getUd(s).v.val;
	if(NULL == ws) {
		return;
	}

	const char *error_str = NULL;
	if(!ws->cb.L) return;
	if(frame){
		byte1 b1;
		uint32_t pos = frame->spos;
		uint32_t size = 1;
		chk_bytechunk *c = chk_bytechunk_read(frame->head,(char*)&b1,&pos,&size);

		if(b1.opcode != TEXT_FRAME || b1.opcode != BINARY_FRAME || b1.opcode != PING_FRAME || b1.opcode != PONG_FRAME) {
			return;
		}

		chk_bytebuffer *data = chk_bytebuffer_new_bychunk(c,pos,frame->datasize - sizeof(b1));
		if(data) {

			const char *opcode = NULL;
			if(b1.opcode == TEXT_FRAME) {
				opcode = text;
			} else if(b1.opcode == BINARY_FRAME) {
				opcode = binary;
			} else if(b1.opcode == PING_FRAME) {
				opcode = ping;
			} else {
				opcode = pong;
			}

	 		luaBufferPusher pusher = {PushBuffer,data};
			error_str = chk_Lua_PCallRef(ws->cb,"sf",opcode,(chk_luaPushFunctor*)&pusher);
			chk_bytebuffer_del(data);
		}
	} else {
		chk_stream_socket_setUd(ws->socket,chk_ud_make_void(NULL));
		chk_stream_socket_close(ws->socket,0);
		ws->socket = NULL;
		error_str = chk_Lua_PCallRef(ws->cb,"ppi",NULL,NULL,error);
		chk_luaRef_release(&ws->cb);
	}

	if(error_str){ 
		CHK_SYSLOG(LOG_ERROR,"error on data_cb %s",error_str);
	}	
}

static int lua_websocket_bind(lua_State *L) {
	chk_event_loop *event_loop;
	websocket *ws = lua_check_websocket(L,1);
	if(!ws->socket){
		return luaL_error(L,"invaild wssocket");
	}
	event_loop = lua_checkeventloop(L,2);
	if(!lua_isfunction(L,3)) 
		return luaL_error(L,"argument 3 of stream_socket_bind must be lua function");
	if(0 != chk_loop_add_handle(event_loop,(chk_handle*)ws->socket,ws_event_cb)) {
		CHK_SYSLOG(LOG_ERROR,"chk_loop_add_handle() failed");
		lua_pushstring(L,"stream_socket_bind failed");
		return 1;
	} else {
		ws->cb = chk_toluaRef(L,3);		
	}
	return 0;
}

static void lua_ws_close_callback(chk_stream_socket *_,chk_ud ud) {
	chk_luaRef cb = ud.v.lr;
	const char *error_str;
	if(!cb.L) return;
	error_str = chk_Lua_PCallRef(cb,"");
	if(error_str) CHK_SYSLOG(LOG_ERROR,"error on lua_ws_close_callback %s",error_str);
	chk_luaRef_release(&cb);
}

static int lua_websocket_set_close_cb(lua_State *L) {
	websocket *ws = lua_check_websocket(L,1);
	if(!ws->socket){
		return 0;
	}

	if(!lua_isfunction(L,2)) 
		return luaL_error(L,"argument 2 of SetCloseCallBack must be lua function");

	chk_luaRef cb = chk_toluaRef(L,2);
	chk_stream_socket_set_close_callback(ws->socket,lua_ws_close_callback,chk_ud_make_lr(cb));
	return 0;
}

static int lua_websocket_getsockaddr(lua_State *L) {
	websocket *ws = lua_check_websocket(L,1);
	if(!ws->socket){
		return luaL_error(L,"invaild websocket");
	}
	chk_sockaddr *addr = LUA_NEWUSERDATA(L,chk_sockaddr);
	if(NULL == addr) {
		return 0;
	}

	if(0 != chk_stream_socket_getsockaddr(ws->socket,addr)){
		return 0;
	}

	luaL_getmetatable(L, SOCK_ADDR_METATABLE);
	lua_setmetatable(L, -2);

	return 1;
}

static int lua_websocket_getpeeraddr(lua_State *L) {
	websocket *ws = lua_check_websocket(L,1);
	if(!ws->socket){
		return luaL_error(L,"invaild websocket");
	}
	chk_sockaddr *addr = LUA_NEWUSERDATA(L,chk_sockaddr);
	if(NULL == addr) {
		return 0;
	}

	if(0 != chk_stream_socket_getpeeraddr(ws->socket,addr)){
		return 0;
	}

	luaL_getmetatable(L, SOCK_ADDR_METATABLE);
	lua_setmetatable(L, -2);

	return 1;	
}

static void register_websocket(lua_State *L) {
	luaL_Reg websocket_mt[] = {
		{"__gc", lua_websocket_gc},
		{NULL, NULL}
	};

	luaL_Reg websocket_methods[] = {
		{"Send",    		lua_websocket_send},
		{"Start",   		lua_websocket_bind},		
		{"Close",   		lua_websocket_close},
		{"GetSockAddr", 	lua_websocket_getsockaddr},
		{"GetPeerAddr", 	lua_websocket_getpeeraddr},	
		{"SetCloseCallBack",lua_websocket_set_close_cb},
//		{"Ping", 			lua_websocket_ping},	
//		{"Pong",			lua_websocket_pong},															
		{NULL,     NULL}
	};

	luaL_newmetatable(L, WEBSOCKET_METATABLE);
	luaL_setfuncs(L, websocket_mt, 0);

	luaL_newlib(L, websocket_methods);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	lua_newtable(L);
	
	lua_pushstring(L,"server");
	lua_newtable(L);
	SET_FUNCTION(L,"Upgrade",lua_server_upgrade);
	lua_settable(L,-3);	

	lua_pushstring(L,"client");
	lua_newtable(L);
	SET_FUNCTION(L,"Upgrade",lua_client_upgrade);
	lua_settable(L,-3);	

}