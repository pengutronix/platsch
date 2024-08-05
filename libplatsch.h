#ifndef __LIBPLATSCH_H__
#define __LIBPLATSCH_H__

struct platsch_ctx;

void platsch_draw(struct platsch_ctx *ctx);
struct platsch_ctx *platsch_create_ctx(const char *dir, const char *base);
struct platsch_ctx *platsch_alloc_ctx(const char *dir, const char *base);
int platsch_init_ctx(struct platsch_ctx *ctx);

void platsch_destroy_ctx(struct platsch_ctx *ctx);

#endif /* __LIBPLATSCH_H__ */
