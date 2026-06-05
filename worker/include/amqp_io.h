#ifndef AMQP_IO_H
#define AMQP_IO_H

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
	const char* exchange;
	const char* task_rk;
	const char* result_rk;
	uint16_t    prefetch;
	int         heartbeat_sec;
} AmqpCfg;

typedef struct AmqpConn AmqpConn;

AmqpConn* amqp_io_connect(const AmqpCfg* cfg);
void      amqp_io_close(AmqpConn* c);

int  amqp_io_consume_one(AmqpConn* c, char** body, size_t* body_len, uint64_t* delivery_tag);
void amqp_io_free_body(char* body);

int amqp_io_publish_result(AmqpConn* c, const char* body, size_t body_len);

int amqp_io_ack(AmqpConn* c, uint64_t delivery_tag);
int amqp_io_nack(AmqpConn* c, uint64_t delivery_tag, int requeue);

#ifdef AMQP_IO_IMPL

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include <amqp.h>
#include <amqp_tcp_socket.h>
#include <amqp_framing.h>

#ifndef AMQP_IO_LOG
#define AMQP_IO_LOG(fmt, ...) fprintf(stderr, "[amqp] " fmt "\n", ##__VA_ARGS__)
#endif

struct AmqpConn {
	amqp_connection_state_t conn;
	amqp_socket_t*          sock;
	AmqpCfg                 cfg;
	int                     consumer_started;
};

static int amqp_io_check_reply(amqp_rpc_reply_t r, const char* what) {
	switch (r.reply_type) {
	case AMQP_RESPONSE_NORMAL:
		return 0;
	case AMQP_RESPONSE_NONE:
		AMQP_IO_LOG("%s: missing rpc reply", what);
		return -1;
	case AMQP_RESPONSE_LIBRARY_EXCEPTION:
		AMQP_IO_LOG("%s: %s", what, amqp_error_string2(r.library_error));
		return -1;
	case AMQP_RESPONSE_SERVER_EXCEPTION: {
		if (r.reply.id == AMQP_CONNECTION_CLOSE_METHOD) {
			amqp_connection_close_t* m = (amqp_connection_close_t*)r.reply.decoded;
			AMQP_IO_LOG("%s: server connection.close %u: %.*s", what, m->reply_code,
				(int)m->reply_text.len, (char*)m->reply_text.bytes);
		} else if (r.reply.id == AMQP_CHANNEL_CLOSE_METHOD) {
			amqp_channel_close_t* m = (amqp_channel_close_t*)r.reply.decoded;
			AMQP_IO_LOG("%s: server channel.close %u: %.*s", what, m->reply_code,
				(int)m->reply_text.len, (char*)m->reply_text.bytes);
		} else {
			AMQP_IO_LOG("%s: unknown server method 0x%08x", what, r.reply.id);
		}
		return -1;
	}
	}
	return -1;
}

AmqpConn* amqp_io_connect(const AmqpCfg* cfg) {
	AmqpConn* c = (AmqpConn*)calloc(1, sizeof(*c));
	c->cfg = *cfg;
	c->conn = amqp_new_connection();
	c->sock = amqp_tcp_socket_new(c->conn);
	if (!c->sock) {
		AMQP_IO_LOG("amqp_tcp_socket_new failed");
		amqp_destroy_connection(c->conn);
		free(c);
		return NULL;
	}
	int s = amqp_socket_open(c->sock, cfg->host, cfg->port);
	if (s != AMQP_STATUS_OK) {
		AMQP_IO_LOG("socket_open(%s:%d): %s", cfg->host, cfg->port, amqp_error_string2(s));
		amqp_destroy_connection(c->conn);
		free(c);
		return NULL;
	}
	if (amqp_io_check_reply(
			amqp_login(c->conn, cfg->vhost ? cfg->vhost : "/", 0, 131072,
			           cfg->heartbeat_sec, AMQP_SASL_METHOD_PLAIN, cfg->user, cfg->pass),
			"login") < 0) {
		amqp_destroy_connection(c->conn);
		free(c);
		return NULL;
	}
	amqp_channel_open(c->conn, 1);
	if (amqp_io_check_reply(amqp_get_rpc_reply(c->conn), "channel.open") < 0) goto fail;

	// declare task queue (durable, not exclusive, not auto-delete)
	amqp_queue_declare(c->conn, 1, amqp_cstring_bytes(cfg->task_queue),
		0, /*passive*/ 1, /*durable*/ 0, /*excl*/ 0 /*autodel*/, amqp_empty_table);
	if (amqp_io_check_reply(amqp_get_rpc_reply(c->conn), "queue.declare task") < 0) goto fail;

	amqp_queue_declare(c->conn, 1, amqp_cstring_bytes(cfg->result_queue),
		0, 1, 0, 0, amqp_empty_table);
	if (amqp_io_check_reply(amqp_get_rpc_reply(c->conn), "queue.declare result") < 0) goto fail;

	// QoS prefetch
	amqp_basic_qos(c->conn, 1, 0, cfg->prefetch ? cfg->prefetch : 1, 0);
	if (amqp_io_check_reply(amqp_get_rpc_reply(c->conn), "basic.qos") < 0) goto fail;

	// publisher confirms
	amqp_confirm_select(c->conn, 1);
	if (amqp_io_check_reply(amqp_get_rpc_reply(c->conn), "confirm.select") < 0) goto fail;

	// start consuming task queue (manual ack)
	amqp_basic_consume(c->conn, 1, amqp_cstring_bytes(cfg->task_queue),
		amqp_empty_bytes, /*no_local*/ 0, /*no_ack*/ 0, /*excl*/ 0, amqp_empty_table);
	if (amqp_io_check_reply(amqp_get_rpc_reply(c->conn), "basic.consume") < 0) goto fail;
	c->consumer_started = 1;

	return c;
fail:
	amqp_channel_close(c->conn, 1, AMQP_REPLY_SUCCESS);
	amqp_connection_close(c->conn, AMQP_REPLY_SUCCESS);
	amqp_destroy_connection(c->conn);
	free(c);
	return NULL;
}

void amqp_io_close(AmqpConn* c) {
	if (!c) return;
	amqp_channel_close(c->conn, 1, AMQP_REPLY_SUCCESS);
	amqp_connection_close(c->conn, AMQP_REPLY_SUCCESS);
	amqp_destroy_connection(c->conn);
	free(c);
}

int amqp_io_consume_one(AmqpConn* c, char** body, size_t* body_len, uint64_t* delivery_tag) {
	amqp_envelope_t env;
	amqp_maybe_release_buffers(c->conn);
	amqp_rpc_reply_t r = amqp_consume_message(c->conn, &env, NULL, 0);
	if (r.reply_type != AMQP_RESPONSE_NORMAL) {
		amqp_io_check_reply(r, "consume_message");
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

void amqp_io_free_body(char* body) { free(body); }

int amqp_io_publish_result(AmqpConn* c, const char* body, size_t body_len) {
	amqp_basic_properties_t props;
	memset(&props, 0, sizeof(props));
	props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
	props.content_type = amqp_cstring_bytes("application/json");
	props.delivery_mode = 2; // persistent

	amqp_bytes_t b;
	b.len = body_len;
	b.bytes = (void*)body;

	int ret = amqp_basic_publish(c->conn, 1,
		amqp_cstring_bytes(c->cfg.exchange ? c->cfg.exchange : ""),
		amqp_cstring_bytes(c->cfg.result_rk ? c->cfg.result_rk : c->cfg.result_queue),
		/*mandatory*/ 0, /*immediate*/ 0, &props, b);
	if (ret != AMQP_STATUS_OK) {
		AMQP_IO_LOG("basic.publish: %s", amqp_error_string2(ret));
		return -1;
	}
	// Wait for confirm (basic.ack) up to 5s.
	struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
	amqp_frame_t frame;
	while (1) {
		int rc = amqp_simple_wait_frame_noblock(c->conn, &frame, &tv);
		if (rc != AMQP_STATUS_OK) {
			AMQP_IO_LOG("wait confirm: %s", amqp_error_string2(rc));
			return -1;
		}
		if (frame.frame_type != AMQP_FRAME_METHOD) continue;
		if (frame.payload.method.id == AMQP_BASIC_ACK_METHOD)  return 0;
		if (frame.payload.method.id == AMQP_BASIC_NACK_METHOD) {
			AMQP_IO_LOG("publish nacked by broker");
			return -1;
		}
		if (frame.payload.method.id == AMQP_BASIC_RETURN_METHOD) continue;
	}
}

int amqp_io_ack(AmqpConn* c, uint64_t delivery_tag) {
	int rc = amqp_basic_ack(c->conn, 1, delivery_tag, 0);
	if (rc != 0) {
		AMQP_IO_LOG("basic.ack: %s", amqp_error_string2(rc));
		return -1;
	}
	return 0;
}

int amqp_io_nack(AmqpConn* c, uint64_t delivery_tag, int requeue) {
	int rc = amqp_basic_nack(c->conn, 1, delivery_tag, 0, requeue ? 1 : 0);
	if (rc != 0) {
		AMQP_IO_LOG("basic.nack: %s", amqp_error_string2(rc));
		return -1;
	}
	return 0;
}

#endif // AMQP_IO_IMPL
#endif // AMQP_IO_H
