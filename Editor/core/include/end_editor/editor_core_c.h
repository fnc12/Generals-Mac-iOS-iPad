#ifndef END_EDITOR_CORE_C_H
#define END_EDITOR_CORE_C_H

/* C ABI of the END editor core. One core per process.
 *
 * OUTBOUND (core -> client): the SQLite file at end_editor_db_path(). The core is its only writer;
 * the client opens its own read connection and reads it each tick. Tables:
 *   status  (one row, id = 1): state ("idle" | "loading" | "ready" | "error"), message, game_dir,
 *           object_count, revision (grows by one on every change, so a client can skip re-reading)
 *   objects: one row per object template of the opened game
 *   warnings: problems found while reading the game data
 *   model   (one row, id = 1): the model in the preview: name, state, message, revision,
 *           bone_count, animation, frame_count, frame_rate, pose (frame_count * bone_count
 *           column-major 4x4 float matrices), visible (one byte per frame and bone), bounds
 *   model_parts: pieces of the model with one texture each: vertices (36-byte records: position,
 *           normal, uv, uint32 bone), indices (uint32 triangle list), hidden, two_sided, blend,
 *           house_color
 *   model_bones, model_textures (RGBA8, rows top to bottom), model_animations
 * Column details: src/storage.h.
 *
 * INBOUND (client -> core): tab separated commands through end_editor_send():
 *   "game.open\t<Zero Hour folder>[\t<base Generals folder>]"
 *       the base game's archives hold most of the models; the first folder wins on clashes
 *   "model.open\t<model name>"        e.g. the Model of an object ("AVLeopard")
 *   "model.animation\t<name>"         one of model_animations, or "" for the bind pose
 * Errors: error:missing_argument, error:unknown_command, error:no_game (model before game).
 * Work runs in end_editor_update(), which the client calls once per tick. */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/* Library version (static string). */
const char *end_editor_get_version(void);

/* Opens (creates) the SQLite store at db_path. NULL or "" uses a file in the temp folder.
 * Returns false if the core is already initialized or the store cannot be opened. */
bool end_editor_initialize(const char *db_path);

/* Closes the store. */
void end_editor_shutdown(void);

/* Path of the SQLite file the client reads. Valid until end_editor_shutdown(). */
const char *end_editor_db_path(void);

/* Queues a command. Returns "" when accepted or "error:<code>"; the string stays valid until the
 * next call. */
const char *end_editor_send(const char *command);

/* Runs queued work. Call once per client tick. */
void end_editor_update(void);

#ifdef __cplusplus
}
#endif

#endif /* END_EDITOR_CORE_C_H */
