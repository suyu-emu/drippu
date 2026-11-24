// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// All blocking calls run on a worker or guest thread. Each exported app signs
// in on the player's device. No exporter credential is included in the build.
int drippu_session_ensure(int interactive);
uint64_t drippu_session_pid(void);
int drippu_session_id_token(char *buffer, size_t capacity, uint64_t *length);
void drippu_session_activity(void);
void drippu_session_logout(void);
const char *drippu_session_error(void);
int drippu_async_mark(void *context, int mark);
void drippu_manage_session(void);
void drippu_install_account_menu(void *menu);
#ifdef __cplusplus
}
#endif
