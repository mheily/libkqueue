#ifndef _DARLING_LISTENREGISTRY_H
#define _DARLING_LISTENREGISTRY_H

#include "../common/private.h"

bool VISIBLE __darling_kqueue_get_listen_status(int fd);
void VISIBLE __darling_kqueue_register_listen(int fd);
void VISIBLE __darling_kqueue_unregister_listen(int fd);

#endif
