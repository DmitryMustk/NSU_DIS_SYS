#ifndef MGR_AMQP_IO_H
#define MGR_AMQP_IO_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
	const char* host;
	int         port;
	const char* user;
	const char* pass;
	const char* vhost;
	const char* task_queue;
	const char* result_queue;
	int         heartbeat_sec;
} AmqpCfg;

typedef struct AmqpPub AmqpPub;
typedef struct AmqpSub AmqpSub;

AmqpPub* amqp_pub_new(const AmqpCfg* cfg);
void     amqp_pub_destroy(AmqpPub*);


int      amqp_pub_publish_task(AmqpPub*, const char* body, size_t body_len);

AmqpSub* amqp_sub_new(const AmqpCfg* cfg);
void     amqp_sub_destroy(AmqpSub*);
int      amqp_sub_consume_one(AmqpSub*, char** body, size_t* body_len, uint64_t* delivery_tag, int timeout_ms);
void     amqp_sub_free_body(char* body);
int      amqp_sub_ack(AmqpSub*, uint64_t delivery_tag);
int      amqp_sub_nack(AmqpSub*, uint64_t delivery_tag, int requeue);

#ifdef MGR_AMQP_IO_IMPL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include <amqp.h>
#include <amqp_tcp_socket.h>
#include <amqp_framing.h>

#ifndef AMQP_LOG
#define AMQP_LOG(fmt, ...) fprintf(stderr, "[amqp] " fmt "\n", ##__VA_ARGS__)
#endif

struct AmqpPub {
	pthread_mutex_t          mu;
	amqp_connection_state_t  conn;
	amqp_socket_t*           sock;
	int                      ok;
	AmqpCfg                  cfg;
};

struct AmqpSub {
	amqp_connection_state_t  conn;
	amqp_socket_t*           sock;
	int                      ok;
	AmqpCfg                  cfg;
};

static int amqp_check(amqp_rpc_reply_t r, const char* what) {
	switch (r.reply_type) {
	case AMQP_RESPONSE_NORMAL: return 0;
	case AMQP_RESPONSE_LIBRARY_EXCEPTION:
		AMQP_LOG("%s: %s", what, amqp_error_string2(r.library_error)); return -1;
	case AMQP_RESPONSE_SERVER_EXCEPTION:
		AMQP_LOG("%s: server exception 0x%08x", what, r.reply.id); return -1;
	default: AMQP_LOG("%s: unknown", what); return -1;
	}
}

static int amqp_setup(amqp_connection_state_t* out_conn, amqp_socket_t** out_sock, const AmqpCfg* cfg) {
	amqp_connection_state_t c = amqp_new_connection();
	amqp_socket_t* s = amqp_tcp_socket_new(c);
	if (!s) { AMQP_LOG("tcp_socket_new failed"); amqp_destroy_connection(c); return -1; }
	int rc = amqp_socket_open(s, cfg->host, cfg->port);
	if (rc != AMQP_STATUS_OK) {
		AMQP_LOG("connect %s:%d: %s", cfg->host, cfg->port, amqp_error_string2(rc));
		amqp_destroy_connection(c); return -1;
	}
	if (amqp_check(amqp_login(c, cfg->vhost ? cfg->vhost : "/", 0, 131072,
	    cfg->heartbeat_sec, AMQP_SASL_METHOD_PLAIN, cfg->user, cfg->pass), "login") < 0) {
		amqp_destroy_connection(c); return -1;
	}
	amqp_channel_open(c, 1);
	if (amqp_check(amqp_get_rpc_reply(c), "channel.open") < 0) {
		amqp_destroy_connection(c); return -1;
	}
	amqp_queue_declare(c, 1, amqp_cstring_bytes(cfg->task_queue),
		0, /*durable*/ 1, 0, 0, amqp_empty_table);
	if (amqp_check(amqp_get_rpc_reply(c), "queue.declare task") < 0) {
		amqp_destroy_connection(c); return -1;
	}
	amqp_queue_declare(c, 1, amqp_cstring_bytes(cfg->result_queue),
		0, /*durable*/ 1, 0, 0, amqp_empty_table);
	if (amqp_check(amqp_get_rpc_reply(c), "queue.declare result") < 0) {
		amqp_destroy_connection(c); return -1;
	}
	*out_conn = c; *out_sock = s;
	return 0;
}

// ---------------- publisher ----------------

static int pub_connect(AmqpPub* p) {
	if (amqp_setup(&p->conn, &p->sock, &p->cfg) != 0) return -1;
	amqp_confirm_select(p->conn, 1);
	if (amqp_check(amqp_get_rpc_reply(p->conn), "confirm.select") < 0) {
		amqp_destroy_connection(p->conn); return -1;
	}
	p->ok = 1;
	AMQP_LOG("pub connected to %s:%d", p->cfg.host, p->cfg.port);
	return 0;
}

static void pub_disconnect_locked(AmqpPub* p) {
	if (!p->ok) return;
	amqp_channel_close(p->conn, 1, AMQP_REPLY_SUCCESS);
	amqp_connection_close(p->conn, AMQP_REPLY_SUCCESS);
	amqp_destroy_connection(p->conn);
	p->ok = 0;
}

AmqpPub* amqp_pub_new(const AmqpCfg* cfg) {
	AmqpPub* p = (AmqpPub*)calloc(1, sizeof(*p));
	pthread_mutex_init(&p->mu, NULL);
	p->cfg = *cfg;
	pub_connect(p);
	return p;
}

void amqp_pub_destroy(AmqpPub* p) {
	if (!p) return;
	pthread_mutex_lock(&p->mu);
	pub_disconnect_locked(p);
	pthread_mutex_unlock(&p->mu);
	pthread_mutex_destroy(&p->mu);
	free(p);
}

int amqp_pub_publish_task(AmqpPub* p, const char* body, size_t body_len) {
	pthread_mutex_lock(&p->mu);
	if (!p->ok) {
		if (pub_connect(p) != 0) { pthread_mutex_unlock(&p->mu); return -1; }
	}
	amqp_basic_properties_t props;
	memset(&props, 0, sizeof(props));
	props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
	props.content_type = amqp_cstring_bytes("application/json");
	props.delivery_mode = 2; // persistent

	amqp_bytes_t b = { .len = body_len, .bytes = (void*)body };
	int rc = amqp_basic_publish(p->conn, 1,
		amqp_cstring_bytes(""), amqp_cstring_bytes(p->cfg.task_queue),
		0, 0, &props, b);
	if (rc != AMQP_STATUS_OK) {
		AMQP_LOG("publish: %s", amqp_error_string2(rc));
		pub_disconnect_locked(p);
		pthread_mutex_unlock(&p->mu);
		return -1;
	}
	struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
	amqp_frame_t frame;
	while (1) {
		int wr = amqp_simple_wait_frame_noblock(p->conn, &frame, &tv);
		if (wr != AMQP_STATUS_OK) {
			AMQP_LOG("wait confirm: %s", amqp_error_string2(wr));
			pub_disconnect_locked(p);
			pthread_mutex_unlock(&p->mu);
			return -1;
		}
		if (frame.frame_type != AMQP_FRAME_METHOD) continue;
		if (frame.payload.method.id == AMQP_BASIC_ACK_METHOD)  { pthread_mutex_unlock(&p->mu); return 0; }
		if (frame.payload.method.id == AMQP_BASIC_NACK_METHOD) { pub_disconnect_locked(p); pthread_mutex_unlock(&p->mu); return -1; }
	}
}

// ---------------- subscriber ----------------

static int sub_connect(AmqpSub* s) {
	if (amqp_setup(&s->conn, &s->sock, &s->cfg) != 0) return -1;
	amqp_basic_qos(s->conn, 1, 0, 32, 0);
	if (amqp_check(amqp_get_rpc_reply(s->conn), "basic.qos") < 0) {
		amqp_destroy_connection(s->conn); return -1;
	}
	amqp_basic_consume(s->conn, 1, amqp_cstring_bytes(s->cfg.result_queue),
		amqp_empty_bytes, 0, 0, 0, amqp_empty_table);
	if (amqp_check(amqp_get_rpc_reply(s->conn), "basic.consume") < 0) {
		amqp_destroy_connection(s->conn); return -1;
	}
	s->ok = 1;
	AMQP_LOG("sub connected, consuming %s", s->cfg.result_queue);
	return 0;
}

AmqpSub* amqp_sub_new(const AmqpCfg* cfg) {
	AmqpSub* s = (AmqpSub*)calloc(1, sizeof(*s));
	s->cfg = *cfg;
	while (sub_connect(s) != 0) {
		AMQP_LOG("sub connect failed; retry in 3s");
		sleep(3);
	}
	return s;
}

void amqp_sub_destroy(AmqpSub* s) {
	if (!s) return;
	if (s->ok) {
		amqp_channel_close(s->conn, 1, AMQP_REPLY_SUCCESS);
		amqp_connection_close(s->conn, AMQP_REPLY_SUCCESS);
		amqp_destroy_connection(s->conn);
	}
	free(s);
}

int amqp_sub_consume_one(AmqpSub* s, char** body, size_t* body_len, uint64_t* delivery_tag, int timeout_ms) {
	if (!s->ok) {
		if (sub_connect(s) != 0) return -1;
	}
	amqp_envelope_t env;
	amqp_maybe_release_buffers(s->conn);
	struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
	amqp_rpc_reply_t r = amqp_consume_message(s->conn, &env, &tv, 0);
	if (r.reply_type != AMQP_RESPONSE_NORMAL) {
		if (r.reply_type == AMQP_RESPONSE_LIBRARY_EXCEPTION && r.library_error == AMQP_STATUS_TIMEOUT) {
			return 1;
		}
		amqp_check(r, "consume_message");
		amqp_destroy_connection(s->conn);
		s->ok = 0;
		return -1;
	}
	*body = (char*)malloc(env.message.body.len + 1);
	memcpy(*body, env.message.body.bytes, env.message.body.len);
	(*body)[env.message.body.len] = '\0';
	*body_len = env.message.body.len;
	*delivery_tag = env.delivery_tag;
	amqp_destroy_envelope(&env);
	return 0;
}

void amqp_sub_free_body(char* body) { free(body); }

int amqp_sub_ack(AmqpSub* s, uint64_t delivery_tag) {
	int rc = amqp_basic_ack(s->conn, 1, delivery_tag, 0);
	if (rc != 0) { AMQP_LOG("ack: %s", amqp_error_string2(rc)); s->ok = 0; return -1; }
	return 0;
}

int amqp_sub_nack(AmqpSub* s, uint64_t delivery_tag, int requeue) {
	int rc = amqp_basic_nack(s->conn, 1, delivery_tag, 0, requeue ? 1 : 0);
	if (rc != 0) { AMQP_LOG("nack: %s", amqp_error_string2(rc)); s->ok = 0; return -1; }
	return 0;
}

#endif // MGR_AMQP_IO_IMPL
#endif // MGR_AMQP_IO_H
