#ifndef __LIBPLATSCH_H__
#define __LIBPLATSCH_H__

#include <stdint.h>

struct platsch_ctx;

struct platsch_draw_buf {
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t size;
	uint32_t format;
	uint32_t fb_id;
	void *fb;
};

typedef void (*custom_draw_cb)(struct platsch_draw_buf *buf, void *priv);

void platsch_draw(struct platsch_ctx *ctx);
void platsch_register_custom_draw_cb(struct platsch_ctx *ctx,
				     custom_draw_cb cb, void *priv);

struct platsch_ctx *platsch_create_ctx(const char *dir, const char *base);
struct platsch_ctx *platsch_alloc_ctx(const char *dir, const char *base);
int platsch_init_ctx(struct platsch_ctx *ctx);

void platsch_destroy_ctx(struct platsch_ctx *ctx);

#endif /* __LIBPLATSCH_H__ */
