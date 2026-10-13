/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2025 Ennebi Elettronica (https://ennebielettronica.com)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <math.h>
#include <stdio.h> // snprintf, used by babel_name_at to build zero-padded folder names
#include <string.h> // strlen, used by babel_decode_output_to_file for variable-length UTF-8 entries
#include "esp_log.h"
#include "tusb.h"
#include "util.h"
#include "utf8-utf16-converter.h"

#define TU_FIELD_SIZE(_type, _field)  (sizeof(((_type *)0)->_field))

#ifdef MTP_ESP_LOG
#undef MTP_ESP_LOG
#endif
#define MTP_ESP_LOG ESP_LOGD

//--------------------------------------------------------------------+
// Dataset
//--------------------------------------------------------------------+

//------------- device info -------------//
#define DEV_INFO_MANUFACTURER   "p2r3"
#define DEV_INFO_MODEL          "USB of Babel"
#define DEV_INFO_VERSION        "1.0"
#define DEV_PROP_FRIENDLY_NAME  "USB of Babel"

//------------- storage info -------------//
#define STORAGE_DESCRIPTION { 'd', 'i', 's', 'k', 0 }
#define VOLUME_IDENTIFIER { 'v', 'o', 'l', 0 }

typedef MTP_STORAGE_INFO_STRUCT(TU_ARRAY_SIZE((uint16_t[]) STORAGE_DESCRIPTION),
                                TU_ARRAY_SIZE(((uint16_t[])VOLUME_IDENTIFIER))
) storage_info_t;

#define CFG_EXAMPLE_MTP_READONLY

storage_info_t storage_info = {
  #ifdef CFG_EXAMPLE_MTP_READONLY
  .storage_type = MTP_STORAGE_TYPE_FIXED_ROM,
  #else
  .storage_type = MTP_STORAGE_TYPE_FIXED_RAM,
  #endif

  .filesystem_type = MTP_FILESYSTEM_TYPE_GENERIC_HIERARCHICAL,
  .access_capability = MTP_ACCESS_CAPABILITY_READ_ONLY_WITHOUT_OBJECT_DELETION,
  .max_capacity_in_bytes = 0, // calculated at runtime
  .free_space_in_bytes = 0, // calculated at runtime
  .free_space_in_objects = 0, // calculated at runtime
  .storage_description = {
    .count = (TU_FIELD_SIZE(storage_info_t, storage_description)-1) / sizeof(uint16_t),
    .utf16 = STORAGE_DESCRIPTION
  },
  .volume_identifier = {
    .count = (TU_FIELD_SIZE(storage_info_t, volume_identifier)-1) / sizeof(uint16_t),
    .utf16 = VOLUME_IDENTIFIER
  }
};

//--------------------------------------------------------------------+
// MTP FILESYSTEM
//--------------------------------------------------------------------+
// only allow to add 1 more object to make it simpler to manage memory
#define FS_MAX_FILE_COUNT 3UL
#define FS_MAX_FILENAME_LEN 16

#ifdef CFG_EXAMPLE_MTP_READONLY
  #define FS_MAX_CAPACITY_BYTES  0
#else
  #define FS_MAX_CAPACITY_BYTES (4 * 1024UL)

  // object data buffer (excluding 2 predefined files) with simple allocation pointer
  uint8_t fs_buf[FS_MAX_CAPACITY_BYTES];
#endif

#define FS_FIXED_DATETIME "20250808T173500.0" // "YYYYMMDDTHHMMSS.s"

// vvv My LittleFS logic
#define MTP_FILENAME_LENGTH 63
// #define MTP_HANDLE_TABLE_SIZE 32
typedef uint32_t fs_handle_t;
#define FS_INVALID_HANDLE ((fs_handle_t)UINT_MAX)

// typedef struct {
//   fs_handle_t handle;             // Handle assigned to this entry.
//   fs_handle_t parent_handle;      // When all bits are set, the parent is root directory
//   bool is_dir;
//   char name[MTP_FILENAME_LENGTH]; // When first character is 0x00, the entry is empty
// } fs_handletable_entry_t;

// Because MTP requires the responder to provide a consistent handle that can be used in any later
// time of a session to manipulate a file, for convenience we just use this mega handle table to
// record all the files we have in the filesystem. It's redundant, (because filesystem should have
// done the bookkeeping!) but because in LittleFS we don't have a global identifier like inodes,
// it's inherently hard for responder to keep all the handle info with a filesystem that can have
// many more files than available RAM. Therefore, our total object count will pretty much be limited
// to the size of this handle table. But considering how many files you can actually fit onto the
// device, 32 objects should be more than enough already.
//
// In this example, we're implementing a simple filesystem with only one layer of directory. The
// reason is to deliberately limit the complexity.
// typedef struct {
//   fs_handletable_entry_t handles[MTP_HANDLE_TABLE_SIZE];

//   uint32_t handles_used;
// } fs_handletable;
// static fs_handletable handle_table;
FILE *current_file = NULL;
fs_handle_t current_handle = FS_INVALID_HANDLE;
size_t current_file_size = 0;
// ^^^ My LittleFS logic

enum {
  SUPPORTED_STORAGE_ID = 0x00010001u // physical = 1, logical = 1
};

static int32_t fs_get_device_info(tud_mtp_cb_data_t* cb_data);
static int32_t fs_open_close_session(tud_mtp_cb_data_t* cb_data);
static int32_t fs_get_storage_ids(tud_mtp_cb_data_t* cb_data);
static int32_t fs_get_storage_info(tud_mtp_cb_data_t* cb_data);
static int32_t fs_get_device_properties(tud_mtp_cb_data_t* cb_data);
static int32_t fs_get_object_handles(tud_mtp_cb_data_t* cb_data);
static int32_t fs_get_object_info(tud_mtp_cb_data_t* cb_data);
static int32_t fs_get_object(tud_mtp_cb_data_t* cb_data);
static int32_t fs_delete_object(tud_mtp_cb_data_t* cb_data);
static int32_t fs_send_object_info(tud_mtp_cb_data_t* cb_data);
static int32_t fs_send_object(tud_mtp_cb_data_t* cb_data);
static int32_t fs_format_store(tud_mtp_cb_data_t* cb_data);

typedef int32_t (*fs_op_handler_t)(tud_mtp_cb_data_t* cb_data);
typedef struct {
  uint32_t op_code;
  fs_op_handler_t handler;
}fs_op_handler_dict_t;

fs_op_handler_dict_t fs_op_handler_dict[] = {
  { MTP_OP_GET_DEVICE_INFO,       fs_get_device_info    },
  { MTP_OP_OPEN_SESSION,          fs_open_close_session }, // init
  { MTP_OP_CLOSE_SESSION,         fs_open_close_session },
  { MTP_OP_GET_STORAGE_IDS,       fs_get_storage_ids       },
  { MTP_OP_GET_STORAGE_INFO,      fs_get_storage_info      },
  { MTP_OP_GET_DEVICE_PROP_DESC,  fs_get_device_properties  },
  { MTP_OP_GET_DEVICE_PROP_VALUE, fs_get_device_properties },
  { MTP_OP_GET_OBJECT_HANDLES,    fs_get_object_handles    }, // ls
  { MTP_OP_GET_OBJECT_INFO,       fs_get_object_info       }, // stat
  { MTP_OP_GET_OBJECT,            fs_get_object            }, // read file
  { MTP_OP_DELETE_OBJECT,         fs_delete_object         },
  { MTP_OP_SEND_OBJECT_INFO,      fs_send_object_info      },
  { MTP_OP_SEND_OBJECT,           fs_send_object           },
  { MTP_OP_FORMAT_STORE,          fs_format_store          },
};

static bool is_session_opened = false;
// static uint32_t send_obj_handle = 0;

//--------------------------------------------------------------------+
//
//--------------------------------------------------------------------+

// static fs_handle_t handle_self_inc = 0;
// static fs_handle_t fs_assign_new_handle(void)
// {
//   // Use incrementing handle for new objects in handle table. MTP does not allow reusing handles of
//   // deleted objects.
//   // When the session is terminated, we can reset it to 0.
//   return ++handle_self_inc;
// }

// static fs_handletable_entry_t *fs_get_handle_entry(fs_handletable *handle_table, fs_handle_t handle)
// {
//   for (int ii = 0; ii < MTP_HANDLE_TABLE_SIZE; ii++) {
//     if (handle_table->handles[ii].handle == handle) {
//       return &handle_table->handles[ii];
//     }
//   }
//   return NULL;
// }

// static void fs_handletable_regenerate(fs_handletable *handle_table) {

//   int curr_handle = fs_assign_new_handle();
//   strncpy(handle_table->handles[curr_handle].name, "dir1", MTP_FILENAME_LENGTH);
//   handle_table->handles[curr_handle].parent_handle = 0;
//   handle_table->handles[curr_handle].handle = curr_handle;
//   handle_table->handles[curr_handle].is_dir = true;

//   curr_handle = fs_assign_new_handle();
//   strncpy(handle_table->handles[curr_handle].name, "dir2", MTP_FILENAME_LENGTH);
//   handle_table->handles[curr_handle].parent_handle = 0;
//   handle_table->handles[curr_handle].handle = curr_handle;
//   handle_table->handles[curr_handle].is_dir = true;

//   curr_handle = fs_assign_new_handle();
//   strncpy(handle_table->handles[curr_handle].name, "file1", MTP_FILENAME_LENGTH);
//   handle_table->handles[curr_handle].parent_handle = 0;
//   handle_table->handles[curr_handle].handle = curr_handle;

//   curr_handle = fs_assign_new_handle();
//   strncpy(handle_table->handles[curr_handle].name, "file2", MTP_FILENAME_LENGTH);
//   handle_table->handles[curr_handle].parent_handle = 0;
//   handle_table->handles[curr_handle].handle = curr_handle;

//   handle_table->handles_used = curr_handle;

  //   int ii = fs_assign_new_handle();
  //   auto root = opendir("/littlefs");
  //   char path_buf[200];
  //   struct dirent *rootitem;
  //   if (root == NULL) {
  //     ESP_LOGE("MtpInit", "Cannot opendir(\"/littlefs\"), got NULL");
  //     return;
  //   }
  //   while ((rootitem = readdir(root)) != NULL) {
  //     // One root item was found. Record it in the handle table
  //     strncpy(handle_table->handles[ii].name, rootitem->d_name, MTP_FILENAME_LENGTH);
  //     handle_table->handles[ii].parent_handle = 0;
  //     handle_table->handles[ii].handle = ii;
  //     MTP_ESP_LOG("MtpInit", "Handle %d = /%s", ii, rootitem->d_name);
  //     if ((ii = fs_assign_new_handle()) >= MTP_HANDLE_TABLE_SIZE) {
  //       ESP_LOGW("MtpInit", "Handle table full, stopping handle table init");
  //       goto cleanup;
  //     }

  //     // If it is a directory, we look inside too
  //     if (rootitem->d_type == DT_DIR) {
  //       auto parent_handle = ii;
  //       handle_table->handles[parent_handle].is_dir = true;

  //       strcpy(path_buf, "/littlefs/");
  //       strcat(path_buf, rootitem->d_name);
  //       auto subdir = opendir(path_buf);
  //       if (subdir == NULL) {
  //         ESP_LOGE("MtpInit", "Cannot opendir(\"/littlefs/%s\"), got NULL", rootitem->d_name);
  //         continue;
  //       }
  //       struct dirent *subdiritem;
  //       while ((subdiritem = readdir(subdir)) != NULL) {
  //         // One subdir item was found. Record it in the handle
  //         strncpy(handle_table->handles[ii].name, subdiritem->d_name, MTP_FILENAME_LENGTH);
  //         handle_table->handles[ii].parent_handle = parent_handle;
  //         handle_table->handles[ii].is_dir = subdiritem->d_type == DT_DIR;
  //         MTP_ESP_LOG("MtpInit", "Handle %d = /%s/%s", ii, rootitem->d_name, subdiritem->d_name);
  //         if ((ii = fs_assign_new_handle()) >= MTP_HANDLE_TABLE_SIZE) {
  //           ESP_LOGW("MtpInit", "Handle table full, stopping handle table init");
  //           goto cleanup;
  //         }
  //       }
  //       closedir(subdir);
  //     }
  //   }
  // cleanup:
  //   closedir(root);
  //   handle_table->handles_used = ii;
// }

// static fs_handle_t fs_handletable_find_empty_entry(const fs_handletable *handle_table) {
//   for (int ii = 0; ii < MTP_HANDLE_TABLE_SIZE; ii++) {
//     if (handle_table->handles[ii].name[0] == '\0') {
//       MTP_ESP_LOG("MtpFS", "Found empty handle slot %d", ii);
//       return ii;
//     }
//   }
//   return FS_INVALID_HANDLE;
// }

// static bool fs_handle_valid(fs_handletable *handle_table, fs_handle_t handle) {
//   for (int ii = 0; ii < MTP_HANDLE_TABLE_SIZE; ++ii) {
//     fs_handletable_entry_t *entry = &handle_table->handles[ii];
//     if (entry->handle == handle && entry->name[0] != '\0') {
//       return true;
//     }
//   }
//   return false;
// }

// static bool fs_path_from_handle(fs_handletable *handle_table, fs_handle_t handle, char *path_out, int buf_len)
// {
//   if (!fs_handle_valid(handle_table, handle)) {
//     return false;
//   }
//   fs_handletable_entry_t *entry = fs_get_handle_entry(handle_table, handle);
//   strlcpy(path_out, "/littlefs/", buf_len);
//   if (entry->parent_handle != 0) {
//     fs_handletable_entry_t *parent_entry = fs_get_handle_entry(handle_table, entry->parent_handle);
//     strlcat(path_out, parent_entry->name, buf_len);
//     strlcat(path_out, "/", buf_len);
//   }
//   strlcat(path_out, entry->name, buf_len);
//   MTP_ESP_LOG("MtpFS", "Mapped Handle %d to File %s", handle, path_out);
//   return true;
// }

// static int fs_path_create(fs_handletable *handle_table, fs_handle_t parent_handle, const char *name, char *path_out, int buf_len)
// {
//   if (parent_handle != 0) {
//     fs_handletable_entry_t *parent_entry = fs_get_handle_entry(handle_table, parent_handle);
//     if (parent_entry == NULL) {
//       return -ENOENT;
//     }
//     strlcpy(path_out, "/littlefs/", buf_len);
//     strlcat(path_out, parent_entry->name, buf_len);
//     strlcat(path_out, "/", buf_len);
//   } else {
//     strlcpy(path_out, "/littlefs/", buf_len);
//   }
//   strlcat(path_out, name, buf_len);
//   return 0;
// }

// static int fs_stat_handle(fs_handletable *handle_table, fs_handle_t handle, struct stat *stat_buf, fs_handletable_entry_t **handletable_entry)
// {
//   char path_buf[200];
//   if (!fs_path_from_handle(handle_table, handle, path_buf, sizeof(path_buf))) {
//     return -EINVAL;
//   }
//   *handletable_entry = fs_get_handle_entry(handle_table, handle);
//   stat_buf->st_size = 1024;
//   stat_buf->st_mode = (*handletable_entry)->is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
//   return 0;
//   // return stat(path_buf, stat_buf);
// }

// static FILE *fs_open_handle(fs_handletable *handle_table, fs_handle_t handle, const char* restrict mode)
// {
//   char path_buf[200];
//   if (current_file != NULL && current_handle == handle) {
//     return current_file;
//   }
//   if (!fs_path_from_handle(handle_table, handle, path_buf, sizeof(path_buf))) {
//     return NULL;
//   }
//   current_handle = handle;
//   current_file = fopen(path_buf, mode);

//   // Resolve file size when in read mode
//   if (mode[0] == 'r') {
//     fseek(current_file, 0, SEEK_END);
//     current_file_size = ftell(current_file);
//     fseek(current_file, 0, SEEK_SET);
//   }

//   return current_file;
// }

// static bool fs_can_create_file(fs_handletable *handle_table, size_t size)
// {
//   size_t capacity_bytes, used_bytes;
//   if (handle_table->handles_used == MTP_HANDLE_TABLE_SIZE) {
//     return false;
//   }
//   esp_littlefs_info("littlefs", &capacity_bytes, &used_bytes);
//   if (capacity_bytes - used_bytes < size) {
//     return false;
//   }
//   return true;
//   return false;
// }

// static fs_handle_t fs_create_file(fs_handletable *handle_table, fs_handle_t parent_handle, const char *name)
// {
//   char pathbuf[200];
//   int retval = fs_path_create(handle_table, parent_handle, name, pathbuf, sizeof(pathbuf));
//   if (retval != 0) {
//     ESP_LOGE("MtpFS", "fs_create_file failed to generate path: %d", retval);
//     return FS_INVALID_HANDLE;
//   }

//   int handle_slot = fs_handletable_find_empty_entry(handle_table);
//   if (handle_slot == FS_INVALID_HANDLE) {
//     ESP_LOGE("MtpFS", "fs_create_file failed to find available entry in handle table");
//     return handle_slot;
//   }

//   fs_handle_t handle = fs_assign_new_handle();

//   current_file = fopen(pathbuf, "w");
//   if (current_file == NULL) {
//     ESP_LOGE("MtpFS", "fs_create_file failed to open file in write mode: %s", pathbuf);
//     return FS_INVALID_HANDLE;
//   }

//   fs_handletable_entry_t *entry = &handle_table->handles[handle_slot];
//   entry->parent_handle = parent_handle;
//   entry->handle = handle;
//   strlcpy(entry->name, name, MTP_FILENAME_LENGTH);

//   current_handle = handle;
//   handle_table->handles_used++;
//   MTP_ESP_LOG("MtpFS", "Created file for write, handle=%d, path=%s", handle, pathbuf);
//   return handle;
// }

// static void fs_close_handle(fs_handle_t handle, FILE *file)
// {
//   if (current_file != file || current_handle != handle) {
//     ESP_LOGE("MtpFS", "fs_close_handle check fail: mismatched state");
//     return;
//   }

//   fclose(file);
//   current_handle = FS_INVALID_HANDLE;
//   current_file = NULL;
// }

// static int fs_delete_handle(fs_handletable *handle_table, fs_handle_t handle)
// {
//   for (int ii = 0; ii < MTP_HANDLE_TABLE_SIZE; ii++) {
//     fs_handletable_entry_t *entry = &handle_table->handles[ii];
//     if (entry->handle == handle && entry->name[0] != '\0') {
//       entry->name[0] = '\0';
//       handle_table->handles_used--;
//       return 0;
//     }
//   }
//   return -ENOENT;
// }

//--------------------------------------------------------------------+
// Control Request callback
//--------------------------------------------------------------------+
bool tud_mtp_request_cancel_cb(tud_mtp_request_cb_data_t* cb_data) {
  mtp_request_reset_cancel_data_t cancel_data;
  memcpy(&cancel_data, cb_data->buf, sizeof(cancel_data));
  (void) cancel_data.code;
  (void ) cancel_data.transaction_id;
  // Dump the file currently working on.
  // fs_close_handle(current_handle, current_file);
  return true;
}

// Invoked when received Device Reset request
// return false to stall the request
bool tud_mtp_request_device_reset_cb(tud_mtp_request_cb_data_t* cb_data) {
  (void) cb_data;
  return true;
}

// Invoked when received Get Extended Event request. Application fill callback data's buffer for response
// return negative to stall the request
int32_t tud_mtp_request_get_extended_event_cb(tud_mtp_request_cb_data_t* cb_data) {
  (void) cb_data;
  return false; // not implemented yet
}

// Invoked when received Get DeviceStatus request. Application fill callback data's buffer for response
// return negative to stall the request
int32_t tud_mtp_request_get_device_status_cb(tud_mtp_request_cb_data_t* cb_data) {
  uint16_t* buf16 = (uint16_t*)(uintptr_t) cb_data->buf;
  buf16[0] = 4; // length
  buf16[1] = MTP_RESP_OK; // status
  return 4;
}

//--------------------------------------------------------------------+
// Bulk Only Protocol
//--------------------------------------------------------------------+
int32_t tud_mtp_command_received_cb(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;
  fs_op_handler_t handler = NULL;
  for (size_t i = 0; i < TU_ARRAY_SIZE(fs_op_handler_dict); i++) {
    if (fs_op_handler_dict[i].op_code == command->header.code) {
      handler = fs_op_handler_dict[i].handler;
      break;
    }
  }

  int32_t resp_code;
  if (handler == NULL) {
    resp_code = MTP_RESP_OPERATION_NOT_SUPPORTED;
  } else {
    resp_code = handler(cb_data);
    if (resp_code > MTP_RESP_UNDEFINED) {
      // send response if needed
      io_container->header->code = (uint16_t)resp_code;
      tud_mtp_response_send(io_container);
    }
  }

  return resp_code;
}

int32_t tud_mtp_data_xfer_cb(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;

  fs_op_handler_t handler = NULL;
  for (size_t i = 0; i < TU_ARRAY_SIZE(fs_op_handler_dict); i++) {
    if (fs_op_handler_dict[i].op_code == command->header.code) {
      handler = fs_op_handler_dict[i].handler;
      break;
    }
  }

  int32_t resp_code;
  if (handler == NULL) {
    resp_code = MTP_RESP_OPERATION_NOT_SUPPORTED;
  } else {
    resp_code = handler(cb_data);
    if (resp_code > MTP_RESP_UNDEFINED) {
      // send response if needed
      io_container->header->code = (uint16_t)resp_code;
      tud_mtp_response_send(io_container);
    }
  }

  return resp_code;
}

int32_t tud_mtp_data_complete_cb(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* resp = &cb_data->io_container;
  switch (command->header.code) {
    case MTP_OP_SEND_OBJECT_INFO: {
      // fs_handletable_entry_t *entry = fs_get_handle_entry(&handle_table, current_handle);
      // if (entry == NULL) {
      //   resp->header->code = MTP_RESP_INVALID_OBJECT_HANDLE;
      //   break;
      // }
      // parameter is: storage id, parent handle, new handle
      mtp_container_add_uint32(resp, SUPPORTED_STORAGE_ID);
      mtp_container_add_uint32(resp, 0);
      mtp_container_add_uint32(resp, current_handle);
      resp->header->code = MTP_RESP_OK;
      break;
    }

    default:
      resp->header->code = (cb_data->xfer_result == XFER_RESULT_SUCCESS) ? MTP_RESP_OK : MTP_RESP_GENERAL_ERROR;
      break;
  }

  tud_mtp_response_send(resp);
  return 0;
}

int32_t tud_mtp_response_complete_cb(tud_mtp_cb_data_t* cb_data) {
  (void) cb_data;
  return 0; // nothing to do
}

// char babel_log[16384] = {0};
// char babel_log[16384] = "Hello, world\n\0";

//--------------------------------------------------------------------+
// File System Handlers
//--------------------------------------------------------------------+
static int32_t fs_get_device_info(tud_mtp_cb_data_t* cb_data) {
  // Device info is already prepared up to playback formats. Application only need to add string fields
  mtp_container_info_t* io_container = &cb_data->io_container;
  mtp_container_add_cstring(io_container, DEV_INFO_MANUFACTURER);
  mtp_container_add_cstring(io_container, DEV_INFO_MODEL);
  mtp_container_add_cstring(io_container, DEV_INFO_VERSION);

  enum { MAX_SERIAL_NCHARS = 32 };
  uint16_t serial_utf16[MAX_SERIAL_NCHARS+1];
  utilGetMacAddressNoDelimiterUtf16le(serial_utf16);
  serial_utf16[tu_min32(12, MAX_SERIAL_NCHARS)] = 0; // ensure null termination
  mtp_container_add_string(io_container, serial_utf16);

  tud_mtp_data_send(io_container);
  return 0;
}

// Mirrors Jorge Luis Borges' "La biblioteca de Babel" as NARRATIVE FLAVOR: the first six levels
// you walk through are always named Hexagono, Pared, Estante, Libro, Pagina, Renglon, in that
// order, with Borges' own counts at each (47 hexagons, 4 walls, 5 shelves, 32 books, 410 pages,
// 40 lines) - but they don't encode anything. babel_compute_index_for skips depths below
// BABEL_DECORATIVE_DEPTH entirely when accumulating the content index, so ANY combination of
// choices through these six levels leads to the exact same file.txt underneath - they're purely
// corridors to wander, giving the "infinite library" feel without any of them being load-bearing
// for the actual message. They also have no file.txt of their own (see babel_level_has_file) -
// reading one partway through a convergent decorative path would be meaningless, so it simply
// isn't offered until the real, numbered levels begin.
//
// The actual message is encoded entirely in the folders AFTER that decorative intro, which are
// all UNIFORM: a leading space plus a direction or turn arrow (e.g. " ↑", see babel_arrow_glyphs
// below), repeating as many times as needed. This MUST stay in lockstep with docs/index.html and
// file-to-path.js.
#define BABEL_DECORATIVE_DEPTH 6
static const char* babel_decorative_names[BABEL_DECORATIVE_DEPTH] = {
  "Hexagono", "Pared", "Estante", "Libro", "Pagina", "Renglon"
};
static const uint32_t babel_decorative_counts[BABEL_DECORATIVE_DEPTH] = {
  47, 4, 5, 32, 410, 40
};
// Real (post-decorative) levels are named " ↑".." ⤷" - a leading space PLUS a direction/turn
// arrow, no number at all, giving a pure maze-navigation theme (8 compass directions, plus 4
// "turn the corner" arrows - see babel_arrow_glyphs below). History: an UNPADDED 8-radix scheme
// using a BARE SINGLE-CHARACTER name (either a plain digit "1".."8" or a single compass arrow
// alone) corrupted content when navigating generated paths on real hardware. Any 2+ character
// name (a zero-padded 2-digit number alone, or a leading space/number before the arrow) fixed it
// reliably - the leading space here is what keeps " ↑" from collapsing back to the single bare
// arrow character known to corrupt. 12 options (up from 8) also cuts the number of folder levels
// a given message needs, since each level now carries slightly more information. MUST stay in
// lockstep with docs/index.html and file-to-path.js.
#define BABEL_CARACTER_COUNT    12

// Separate, much smaller alphabet for the DECODED FILE CONTENT (what ends up inside file.txt) -
// lowercase-only letters (no uppercase), space, and minimal punctuation (just "." and ","). No
// digits and no accented letters - closer to Borges' own story (the library's language uses a
// fixed, minimal set of "orthographic symbols": 22 letters, space, comma, period), and it
// shortens the folder-path needed per character, since a smaller alphabet means fewer bits to
// encode per character (fewer levels needed for the same message). Any uppercase typed by the
// user is folded to lowercase before encoding (see sanitizeContentText in docs/index.html /
// file-to-path.js) rather than dropped. Any accented letter or digit typed is simply dropped
// (also in sanitizeContentText). MUST stay in lockstep with docs/index.html and file-to-path.js.
#define BABEL_CONTENT_ALPHABET_LENGTH 29
const char* babel_content_alphabet[BABEL_CONTENT_ALPHABET_LENGTH] = {
  " ",
  "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
  "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
  ".", ","
};

// Returns the branch count (number of subdirectories, NOT counting the trailing "file.txt"
// leaf, if any - see babel_level_has_file) for the given 0-indexed depth from the root.
static uint32_t babel_branch_at(uint32_t depth) {
  if (depth < BABEL_DECORATIVE_DEPTH) return babel_decorative_counts[depth];
  return BABEL_CARACTER_COUNT;
}

// Whether the level at this depth offers a "file.txt" alongside its subfolder choices.
// Decorative levels don't (see the design note above) - only once the real, numbered levels
// begin does every listing include one.
static bool babel_level_has_file(uint32_t depth) {
  return depth >= BABEL_DECORATIVE_DEPTH;
}

uint16_t babel_name_buf[16] = {0};

// How many digits `max_value` needs in base 10 - used to zero-pad folder numbers so Explorer's
// (lexicographic, NOT numeric) default sort order matches numeric order exactly. Without this,
// "17" sorts between "16" and "170"-style neighbors in a list that also contains "2".."9", while
// visually a human scanning the list expects "9" to come right after "8" - debug captures showed
// exactly this kind of plausible-looking-but-wrong folder getting opened instead of the one
// actually named in the generated path. Zero-padding (e.g. "01".."20") makes string order and
// numeric order identical, removing that whole class of mis-click.
static int babel_digit_width(uint32_t max_value) {
  int width = 1;
  uint32_t threshold = 10;
  while (max_value >= threshold) {
    width++;
    threshold *= 10;
  }
  return width;
}

// Writes "<prefix><zero-padded number>" (ASCII, widened to UTF-16) into babel_name_buf, where an
// empty prefix just gives a plain number (used for the real, post-decorative numbered levels).
// `width` is the minimum digit count (see babel_digit_width) - MUST match exactly what
// docs/index.html and file-to-path.js generate, since MTP path resolution needs an exact string
// match. Returns the number of UTF-16 units written (excluding the null terminator).
static int babel_write_prefixed_number(const char* prefix, uint32_t number, int width) {
  int n = 0;
  for (const char* p = prefix; *p; p++) {
    babel_name_buf[n++] = (uint16_t)(uint8_t)(*p);
  }
  char digits[16];
  int dn = 0;
  if (number == 0) {
    digits[dn++] = '0';
  } else {
    while (number > 0) {
      digits[dn++] = (char)('0' + (number % 10));
      number /= 10;
    }
  }
  while (dn < width) {
    digits[dn++] = '0'; // zero-pad: appended here (LSB-first order), ends up on the LEFT once reversed below
  }
  for (int i = dn - 1; i >= 0; i--) {
    babel_name_buf[n++] = (uint16_t)(uint8_t) digits[i];
  }
  babel_name_buf[n] = 0;
  return n;
}

// 8 compass directions plus 4 "turn the corner" arrows, each a single UTF-16 code unit (all
// within the BMP, well below the surrogate-pair range) - appended after a leading space, e.g.
// " ↑" or " ⤷". Order here MUST exactly match the ARROW_GLYPHS array in docs/index.html and
// file-to-path.js.
static const uint16_t babel_arrow_glyphs[BABEL_CARACTER_COUNT] = {
  0x2191, // ↑ up
  0x2193, // ↓ down
  0x2190, // ← left
  0x2192, // → right
  0x2196, // ↖ up-left
  0x2197, // ↗ up-right
  0x2199, // ↙ down-left
  0x2198, // ↘ down-right
  0x2934, // ⤴ turn: right then curve up
  0x2935, // ⤵ turn: right then curve down
  0x2936, // ⤶ turn: down then curve left
  0x2937  // ⤷ turn: down then curve right
};

// Writes the display name for local_index (0-based, < babel_branch_at(depth)) into
// babel_name_buf. Returns the number of UTF-16 units written (excluding the null).
static int babel_name_at(uint32_t depth, uint32_t local_index) {
  if (depth < BABEL_DECORATIVE_DEPTH) {
    int width = babel_digit_width(babel_branch_at(depth));
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "%s-", babel_decorative_names[depth]);
    return babel_write_prefixed_number(prefix, local_index + 1, width);
  }
  babel_name_buf[0] = (uint16_t)' ';
  babel_name_buf[1] = babel_arrow_glyphs[local_index];
  babel_name_buf[2] = 0;
  return 2;
}

// output_file accumulates, as you descend, the "index" (per the same bijective big-number
// scheme file-to-path.js uses to go the other way: index = sum (charIndex[i]+1)*117^i, over
// each character of the sanitized input text mapped through babel_content_alphabet). Because
// each directory level can now have a DIFFERENT width, this can no longer be done with the
// original's single multiply-and-add-a-digit trick (that trick only works when every level
// has the same width, which let it place the most-significant digit at the root without
// knowing the total path length in advance). Instead, output_file accumulates a weighted
// sum (output_file += (digit+1) * weight_file; weight_file *= radix), least-significant
// digit first - i.e. root-to-leaf, which is exactly the order the firmware naturally visits
// folders in, needing nothing but "how deep am I" to know the radix at each step.
//
// decoded_file holds the ACTUAL file content, computed on demand from output_file (via the
// bijective base-117 decode, the exact inverse of file-to-path.js's/the website's encode) whenever a
// "file.txt" object is actually read - not kept in sync on every descent, since decoding is
// only needed when a file is opened, and changing output_file never invalidates it until
// decode is re-run.
uint8_t output_file[4096] = {0};
size_t output_file_len = 0;
uint8_t weight_file[4096] = {1};
size_t weight_file_len = 1;
// Sized larger than output_file/weight_file: accented letters are 2 UTF-8 bytes each, so the
// worst case (the website's full 3000-character input, entirely accented letters) needs up to
// 6000 bytes here - 8192 keeps a safe margin without being wasteful.
uint8_t decoded_file[8192] = {0};
size_t decoded_file_len = 0;

// Handles are table indices into a persistent, append-only BLOCK table - NOT pure arithmetic
// (depth, local-choice) pairs, and NOT resolved relative to "wherever we last navigated to".
// Each GetObjectHandles call allocates a fresh contiguous block of handles for the requested
// parent's children, permanently recording that block's own parent handle and depth. This is
// what lets GetObjectInfo/GetObject resolve ANY handle - both its displayed name AND its full
// ancestor chain for content - purely from the handle table, with zero dependency on shared
// "current position" state.
//
// That independence matters because Windows does NOT only send MTP commands that follow the
// user's own clicks: background services - the Windows Search content indexer in particular,
// which indexes .txt file contents by default - independently enumerate and read file.txt
// objects as their own, fully separate transactions, interleaved in time with whatever the user
// is doing (each individual transaction is atomic per TinyUSB's MTP driver, but nothing stops
// an unrelated transaction from landing between two of the user's own). An earlier version of
// this file tracked "current position" in a single global (nav_stack + a current depth), kept
// in sync by every GetObjectHandles/GetObject call; that global got silently stomped by exactly
// this kind of unrelated background traffic, corrupting the user's own navigation (hence folders
// appearing empty or showing the wrong content after some browsing). With a handle table, there
// IS no "current position" to stomp - every operation is a pure function of the handle(s) given.
//
// Listing the SAME parent twice returns the exact SAME block (see babel_find_children_block) -
// blocks are memoized, not reallocated per call. This matters beyond just saving table space:
// Windows' MTP/WPD shell extension caches object handles per visual item, and can hand back a
// handle from an earlier visit when the user returns to "the same" folder. If re-listing ever
// allocated a fresh block, that stale-but-still-valid cached handle could end up supplying a
// DIFFERENT (older) ancestor for part of a chain than the one the user actually navigated this
// time - corrupting exactly the early/cached part of a path while deeper, freshly-discovered
// levels stay correct. Memoizing means there's only ever one true block per logical parent, so
// no cached handle can ever disagree with a fresh one about what it points to. Blocks themselves
// are still never deleted (handles stay valid forever once issued).
typedef struct {
  uint32_t parent_handle; // the object handle whose children this block represents (0xFFFFFFFF = root)
  uint32_t depth;         // depth of every entry IN this block (parent's depth + 1; root's children are depth 0)
  uint32_t start;         // first child handle in this contiguous block (handles start <= h < start+count)
  uint32_t count;         // one slot per subfolder choice, plus one more for file.txt if babel_level_has_file(depth)
} babel_block_t;

// Never explicitly reset after this (correctly zero/one-initialized here, at firmware start) -
// see the note in fs_open_close_session for why the table deliberately persists for the whole
// device power-on lifetime rather than being wiped per MTP session.
//
// Sized to comfortably cover the website's 3000-character input cap: at ~5.88 bits/character
// (log2(59), the content alphabet size) and ~6.52 bits/level average (the structural+numbered-
// folder cycle), 3000 characters needs roughly 2700 levels in a straight-line path. 4096 leaves
// real margin for exploration/backtracking on top of that, while keeping this table's footprint
// (4096 * 16 bytes = 64KB) reasonable on a chip with no PSRAM - this previously sat at 8192
// (128KB), a needlessly large chunk of the ESP32-S3's internal SRAM for what any real message
// actually needs, and shrinking it is a safe hedge against any RAM-pressure-related corruption.
#define BABEL_MAX_BLOCKS 4096
babel_block_t babel_blocks[BABEL_MAX_BLOCKS];
uint32_t babel_blocks_len = 0;
uint32_t babel_next_handle = 1; // handle 0 is never issued (root is addressed as 0xFFFFFFFF instead)

// Binary search for the block containing `handle` (blocks are appended in strictly increasing
// `start` order, since handles only ever go up). Returns NULL if handle was never issued.
static babel_block_t* babel_find_block(uint32_t handle) {
  int32_t lo = 0, hi = (int32_t) babel_blocks_len - 1;
  babel_block_t* found = NULL;
  while (lo <= hi) {
    int32_t mid = (lo + hi) / 2;
    if (babel_blocks[mid].start <= handle) {
      found = &babel_blocks[mid];
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return (found && handle < found->start + found->count) ? found : NULL;
}

// Returns the index of the EXISTING block for parent_handle's children, or -1 if that parent
// has never been listed before. Memoizing this way (instead of always allocating a fresh block)
// matters because Windows' MTP/WPD shell extension caches object handles per visual item across
// a session (for icons, thumbnails, its own namespace identity) and can legitimately reuse a
// handle from an earlier visit to "the same" folder when the user later opens a file.txt deep
// inside it. If each re-listing got a brand-new block, a stale-but-still-valid cached handle
// from an earlier (possibly different) exploration could end up supplying the wrong ancestor for
// part of the chain - corrupting exactly the ancestor levels that were cached early, while
// correctly-fresh deeper levels still decode fine (matching exactly the "first few characters
// wrong, rest correct" pattern this was built to fix). Memoizing means a given parent_handle
// ALWAYS maps to the one single, canonical children block - no matter when or how many times
// it's revisited, there's only ever one true answer.
static int32_t babel_find_children_block(uint32_t parent_handle) {
  for (uint32_t i = 0; i < babel_blocks_len; i++) {
    if (babel_blocks[i].parent_handle == parent_handle) {
      return (int32_t) i;
    }
  }
  return -1;
}

// Writes the (possibly memoized) block of handles for parent_handle's children to
// *out_start/*out_branch (the block's actual handle count may be branch or branch+1 depending on
// babel_level_has_file - see babel_block_t.count). Returns false (writing nothing) if
// parent_handle is neither the root sentinel NOR a handle we can actually resolve in the table.
//
// That failure case matters a lot: it used to be silently treated as "must be a root-level
// child" (falling back to depth 0 whenever babel_find_block came back empty). A non-root
// parent_handle genuinely failing to resolve shouldn't happen now that the table persists for
// the device's whole power-on lifetime (see fs_open_close_session), but defending against it
// anyway is cheap insurance: silently guessing "depth 0" for an unresolvable handle would create
// a real block at the WRONG depth, permanently misattributing every level built on top of it -
// exactly the "early/ancestor levels wrong, later/freshly-discovered levels correct" pattern
// this was fixed to prevent. Returning an error instead forces the host to recover by re-listing
// from a handle it still actually has, rather than silently fabricating a wrong structure.
static bool babel_list_children(uint32_t parent_handle, uint32_t* out_start, uint32_t* out_branch, uint32_t* out_handle_count) {
  int32_t existing = babel_find_children_block(parent_handle);
  if (existing >= 0) {
    // NOT `count - 1`: count only equals branch+1 when babel_level_has_file(depth) is true -
    // decorative levels have count == branch exactly (no file slot), so deriving branch that way
    // would be off by one for them. Re-deriving from depth is correct unconditionally.
    *out_branch = babel_branch_at(babel_blocks[existing].depth);
    *out_start = babel_blocks[existing].start;
    *out_handle_count = babel_blocks[existing].count;
    return true;
  }

  uint32_t depth;
  if (parent_handle == 0xFFFFFFFF) {
    depth = 0;
  } else {
    babel_block_t* parent_block = babel_find_block(parent_handle);
    if (!parent_block) {
      return false; // unknown, non-root handle - do NOT guess its depth
    }
    depth = parent_block->depth + 1;
  }
  uint32_t branch = babel_branch_at(depth);
  *out_branch = branch;
  uint32_t count = branch + (babel_level_has_file(depth) ? 1 : 0);
  *out_handle_count = count;

  if (babel_blocks_len >= BABEL_MAX_BLOCKS) {
    *out_start = babel_blocks[BABEL_MAX_BLOCKS - 1].start;
    return true;
  }

  uint32_t start = babel_next_handle;
  babel_blocks[babel_blocks_len++] = (babel_block_t){
    .parent_handle = parent_handle,
    .depth = depth,
    .start = start,
    .count = count
  };
  babel_next_handle += count;
  *out_start = start;
  return true;
}

// Resolves a handle to (depth, local_choice [1-based], branch), purely from the handle table -
// no dependency on navigation history. Returns false if the handle was never issued.
static bool babel_resolve_handle(uint32_t handle, uint32_t* out_depth, uint32_t* out_local_choice, uint32_t* out_branch) {
  babel_block_t* block = babel_find_block(handle);
  if (!block) return false;
  *out_depth = block->depth;
  *out_local_choice = handle - block->start + 1;
  *out_branch = babel_branch_at(block->depth);
  return true;
}

// dst (big-endian... no, little-endian byte array, dst[0] = least significant byte) *= scalar
static void babel_buf_mul_scalar(uint8_t* buf, size_t* len, uint32_t scalar) {
  size_t index = 0;
  uint32_t carry = 0;
  while (index < *len || carry) {
    uint32_t digit = buf[index] * scalar + carry;
    buf[index] = digit & 0xFF;
    carry = digit >> 8;
    if (++index > *len) {
      buf[index] = 0;
      *len = index;
    }
  }
}

// dst += src * scalar (multiply-accumulate; src is left untouched)
static void babel_buf_mac(uint8_t* dst, size_t* dst_len, const uint8_t* src, size_t src_len, uint32_t scalar) {
  size_t index = 0;
  uint32_t carry = 0;
  while (index < src_len || carry) {
    uint32_t src_val = (index < src_len) ? src[index] : 0;
    uint32_t digit = dst[index] + src_val * scalar + carry;
    dst[index] = digit & 0xFF;
    carry = digit >> 8;
    if (++index > *dst_len) {
      dst[index] = 0;
      *dst_len = index;
    }
  }
}

// buf /= scalar (standard long division, most-significant byte first), returns buf % scalar.
// Unlike babel_buf_mul_scalar/babel_buf_mac (which work a byte at a time because the multiplier
// there is itself byte-sized, i.e. base 256), this is a genuine division of the arbitrary-
// precision number by a scalar that does NOT divide 256 evenly (e.g. 59), needed because the
// content alphabet's radix no longer lines up with the byte-array's own base-256 storage.
static uint32_t babel_buf_divmod_scalar(uint8_t* buf, size_t* len, uint32_t scalar) {
  uint32_t remainder = 0;
  for (size_t i = *len; i-- > 0; ) {
    uint32_t cur = (remainder << 8) | buf[i];
    buf[i] = (uint8_t)(cur / scalar);
    remainder = cur % scalar;
  }
  while (*len > 0 && buf[*len - 1] == 0) {
    (*len)--;
  }
  return remainder;
}

// buf -= 1, with full (standard) borrow propagation. Used only by the decode step below.
static void babel_buf_dec_standard(uint8_t* buf, size_t* len) {
  size_t i = 0;
  while (buf[i] == 0) {
    buf[i] = 255;
    i++;
  }
  buf[i]--;
  while (*len > 0 && buf[*len - 1] == 0) {
    (*len)--;
  }
}

// Folds one more path level into the running index, exactly mirroring file-to-path.js's
// own encode loop in reverse: output_file += (local_choice) * weight_file; weight_file *= radix.
// local_choice is the 1-based local index chosen at the level with the given radix.
static void fs_append_output(uint32_t local_choice, uint32_t radix) {
  babel_buf_mac(output_file, &output_file_len, weight_file, weight_file_len, local_choice);
  babel_buf_mul_scalar(weight_file, &weight_file_len, radix);
}

// Computes output_file/weight_file fresh for the folder identified by `parent_handle`
// (0xFFFFFFFF = root), by walking the handle table from that handle up to the root (collecting
// each ancestor's local choice along the way) and replaying those choices root-to-leaf. This
// has NO dependency on any shared "current position" - it's a pure function of parent_handle
// and the (permanent, append-only) handle table, so it's immune to whatever unrelated
// GetObjectHandles/GetObject calls (e.g. background content indexing elsewhere in the tree)
// happened in between. O(depth) handle-table lookups - only runs once per file.txt read, never
// in a hot loop, so this is cheap even for a few thousand levels.
//
// Returns false if the walk hits an ancestor handle we can't resolve before reaching the root
// sentinel. Shouldn't happen now that the table persists for the device's whole power-on
// lifetime (see fs_open_close_session), but this MUST NOT be silently treated as "that's the
// root, stop here" if it ever does: doing so would replay the remaining,
// still-resolvable choices using the WRONG radixes (whatever depth each happens to land on in
// the truncated chain instead of its true depth), corrupting exactly the early/ancestor part of
// the decoded content while the deeper, freshly-discovered part stays correct by coincidence.
// A static (not stack-local) scratch array, which would otherwise risk overflowing a FreeRTOS
// task's stack if declared as a local variable. Matches BABEL_MAX_BLOCKS's sizing rationale -
// 4096 comfortably covers the website's 3000-character cap (~2700 levels) with margin.
#define BABEL_MAX_CHAIN_DEPTH 4096
uint32_t babel_chain_scratch[BABEL_MAX_CHAIN_DEPTH];
static bool babel_compute_index_for(uint32_t parent_handle) {
  uint32_t* chain = babel_chain_scratch; // collected leaf-to-root: local choice at each step
  uint32_t chain_len = 0;

  uint32_t handle = parent_handle;
  while (handle != 0xFFFFFFFF) {
    if (chain_len >= BABEL_MAX_CHAIN_DEPTH) {
      return false; // pathologically deep - refuse rather than silently truncate
    }
    babel_block_t* block = babel_find_block(handle);
    if (!block) {
      return false; // unresolvable ancestor - do NOT guess, refuse to serve wrong content
    }
    chain[chain_len++] = handle - block->start + 1;
    handle = block->parent_handle;
  }

  memset(output_file, 0, sizeof(output_file));
  output_file_len = 0;
  memset(weight_file, 0, sizeof(weight_file));
  weight_file[0] = 1;
  weight_file_len = 1;

  // Replay root-to-leaf: chain[chain_len-1] is the root-level (depth 0) choice. Decorative
  // levels (depth < BABEL_DECORATIVE_DEPTH) are skipped entirely here since they're excluded
  // from the weighted sum by construction (the loop below starts at BABEL_DECORATIVE_DEPTH),
  // regardless of their own branch sizes - they carry zero information either way. The first
  // REAL digit (depth == BABEL_DECORATIVE_DEPTH) must see weight == 1, exactly matching the
  // website's/file-to-path.js's encode, which never touches the index while emitting the
  // decorative prefix either.
  for (uint32_t depth = BABEL_DECORATIVE_DEPTH; depth < chain_len; depth++) {
    fs_append_output(chain[chain_len - 1 - depth], babel_branch_at(depth));
  }
  return true;
}

// Decodes output_file (currently holding "index") into decoded_file (the true file content),
// via the bijective base-117 decode: while (v>0) { v -= 1; digit = v % 117; v /= 117; emit
// babel_content_alphabet[digit]; } - the exact inverse of file-to-path.js's/the website's
// per-character encode. Operates on a scratch copy so output_file/weight_file are left
// untouched - the user may keep navigating deeper (and read other "file.txt" objects along
// the way) after this.
//
// Each alphabet entry can be 1 byte (plain ASCII) or 2 bytes (accented letters, in UTF-8) - NOT
// a fixed 1 byte per digit like the earlier single-byte-alphabet version, so this copies
// strlen(entry) bytes per digit rather than assigning a single byte directly. The bounds check
// is defensive: decoded_file is sized (see its declaration) to comfortably fit the worst case
// (every character 2 bytes) for the website's maximum input length, so this should never
// actually trigger - but if some future change ever grew either limit without the other keeping
// pace, silently stopping early beats corrupting adjacent memory.
uint8_t babel_decode_scratch[4096];
static void babel_decode_output_to_file(void) {
  size_t len = output_file_len;
  memcpy(babel_decode_scratch, output_file, len);
  decoded_file_len = 0;
  while (len > 0) {
    babel_buf_dec_standard(babel_decode_scratch, &len);
    uint32_t digit = babel_buf_divmod_scalar(babel_decode_scratch, &len, BABEL_CONTENT_ALPHABET_LENGTH);
    const char* ch = babel_content_alphabet[digit];
    size_t ch_len = strlen(ch);
    if (decoded_file_len + ch_len > sizeof(decoded_file)) {
      break;
    }
    memcpy(decoded_file + decoded_file_len, ch, ch_len);
    decoded_file_len += ch_len;
  }
}

static int32_t fs_open_close_session(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  if (command->header.code == MTP_OP_OPEN_SESSION) {
    if (is_session_opened) {
      return MTP_RESP_SESSION_ALREADY_OPEN;
    }
    is_session_opened = true;
    // Deliberately NOT resetting the handle table (babel_blocks) or output_file/weight_file
    // here. Windows can legitimately close and reopen the MTP session mid-browse (not
    // necessarily implying the user unplugged the device), and if a session reopen wiped the
    // table while the host still held handles from before it, those stale-but-still-presented
    // handles would become unresolvable - previously this was silently (and wrongly) treated as
    // "must be a root-level child", corrupting content. The handle table instead persists for
    // the device's entire power-on lifetime (it's already correctly zero/one-initialized at
    // firmware start, needing no explicit reset), so a session reopen can never invalidate a
    // handle the host is still holding.
  } else { // close session
    if (!is_session_opened) {
      return MTP_RESP_SESSION_NOT_OPEN;
    }
    is_session_opened = false;
  }
  return MTP_RESP_OK;
}

static int32_t fs_get_storage_ids(tud_mtp_cb_data_t* cb_data) {
  mtp_container_info_t* io_container = &cb_data->io_container;
  uint32_t storage_ids [] = { SUPPORTED_STORAGE_ID };
  mtp_container_add_auint32(io_container, 1, storage_ids);
  tud_mtp_data_send(io_container);
  return 0;
}

static int32_t fs_get_storage_info(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;
  const uint32_t storage_id = command->params[0];
  TU_VERIFY(SUPPORTED_STORAGE_ID == storage_id, -1);
  // update storage info with current free space
  // size_t capacity_bytes, used_bytes;
  // esp_littlefs_info("littlefs", &capacity_bytes, &used_bytes);
  // storage_info.max_capacity_in_bytes = capacity_bytes;
  // storage_info.free_space_in_objects = MTP_HANDLE_TABLE_SIZE - handle_table.handles_used;
  // storage_info.free_space_in_bytes = capacity_bytes - used_bytes;
  storage_info.max_capacity_in_bytes = ~0 / 2;
  storage_info.free_space_in_objects = 0;
  storage_info.free_space_in_bytes = 0;
  mtp_container_add_raw(io_container, &storage_info, sizeof(storage_info));
  tud_mtp_data_send(io_container);
  return 0;
}

static int32_t fs_get_device_properties(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;
  const uint16_t dev_prop_code = (uint16_t) command->params[0];

  if (command->header.code == MTP_OP_GET_DEVICE_PROP_DESC) {
    // get describing dataset
    mtp_device_prop_desc_header_t device_prop_header;
    device_prop_header.device_property_code = dev_prop_code;
    switch (dev_prop_code) {
      case MTP_DEV_PROP_DEVICE_FRIENDLY_NAME:
        device_prop_header.datatype = MTP_DATA_TYPE_STR;
        device_prop_header.get_set = MTP_MODE_GET;
        mtp_container_add_raw(io_container, &device_prop_header, sizeof(device_prop_header));
        mtp_container_add_cstring(io_container, DEV_PROP_FRIENDLY_NAME); // factory
        mtp_container_add_cstring(io_container, DEV_PROP_FRIENDLY_NAME); // current
        mtp_container_add_uint8(io_container, 0); // no form
        tud_mtp_data_send(io_container);
        break;

      default:
        return MTP_RESP_PARAMETER_NOT_SUPPORTED;
    }
  } else {
    // get value
    switch (dev_prop_code) {
      case MTP_DEV_PROP_DEVICE_FRIENDLY_NAME:
        mtp_container_add_cstring(io_container, DEV_PROP_FRIENDLY_NAME);
        tud_mtp_data_send(io_container);
        break;

      default:
        return MTP_RESP_PARAMETER_NOT_SUPPORTED;
    }
  }
  return 0;
}

// Transaction-scoped: set by the COMMAND phase below, read back by its own DATA phase
// continuation calls. Safe to share as plain globals (unlike the old nav_stack) because
// TinyUSB's MTP driver guarantees a transaction's COMMAND and DATA phases are never
// interleaved with a different command - see the design note above babel_block_t.
static uint32_t babel_listing_start = 0;
static uint32_t babel_listing_branch = 0;
static uint32_t babel_listing_handle_count = 0;

static int32_t fs_get_object_handles(tud_mtp_cb_data_t* cb_data) {
  // `ls /<folder_in_question>`
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;

  const uint32_t storage_id = command->params[0];
  const uint32_t obj_format = command->params[1]; // optional
  const uint32_t parent_handle = command->params[2];
  (void)obj_format;

  if (storage_id != 0xFFFFFFFF && storage_id != SUPPORTED_STORAGE_ID) {
    return MTP_RESP_INVALID_STORAGE_ID;
  }

  if (cb_data->phase == MTP_PHASE_COMMAND) {
    if (!babel_list_children(parent_handle, &babel_listing_start, &babel_listing_branch, &babel_listing_handle_count)) {
      return MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    const uint32_t handle_count = babel_listing_handle_count;

    // Payload space in the first packet (after the 12-byte container header)
    const uint32_t first_payload_bytes = io_container->payload_bytes;

    // Write the MTP array count field (4 bytes) into the payload
    uint32_t* p = (uint32_t*) io_container->payload;
    *p++ = handle_count;
    io_container->header->len += 4;

    // Fill the remaining space in the first packet with as many handles as fit
    const uint32_t first_batch = tu_min32(handle_count, (first_payload_bytes - 4) / 4);
    for (uint32_t i = 0; i < first_batch; i++) {
      *p++ = babel_listing_start + i;
    }

    // Declare length of full buffer to be streamed
    io_container->header->len += handle_count * 4;

    tud_mtp_data_send(io_container);
  } else { // MTP_PHASE_DATA
    const uint32_t handle_count = babel_listing_handle_count;
    const uint32_t data_sent = cb_data->total_xferred_bytes - sizeof(mtp_container_header_t);
    const uint32_t handles_sent = (data_sent - 4) / 4; // how many handles already sent
    const uint32_t handles_remaining = handle_count - handles_sent;
    const uint32_t handles_this_packet = tu_min32(handles_remaining, io_container->payload_bytes / 4);

    uint32_t* p = (uint32_t*) io_container->payload;
    for (uint32_t i = 0; i < handles_this_packet; i++) {
      p[i] = babel_listing_start + handles_sent + i;
    }

    tud_mtp_data_send(io_container);
  }

  return 0;
}

uint16_t babel_file_name[] = {'f','i','l','e','.','t','x','t',0};
#define BABEL_FILE_NAME_LENGTH 8

// Returns the display name for local_index (0-based) at the given depth, writing into
// babel_name_buf when it's a subdirectory. *out_len excludes the null terminator. Depends only
// on (depth, local_index, branch) - never on navigation history - so GetObjectInfo can call
// this for ANY handle at ANY time and always get the correct name.
static uint16_t* babel_entry_name(uint32_t depth, uint32_t local_index, uint32_t branch, int* out_len) {
  if (local_index == branch) {
    *out_len = BABEL_FILE_NAME_LENGTH;
    return babel_file_name;
  }
  *out_len = babel_name_at(depth, local_index);
  return babel_name_buf;
}

static int32_t fs_get_object_info(tud_mtp_cb_data_t* cb_data) {
  // `ls -l /<file_in_question>`
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;
  const uint32_t obj_handle = command->params[0];

  // Fully stateless: the name of ANY handle depends only on its own handle-table entry, never
  // on whatever we last navigated to. This is what makes it safe for the host to ask about
  // handles in any order (cached, revisited, interleaved with other folders, etc.).
  uint32_t depth, local_choice, branch;
  if (!babel_resolve_handle(obj_handle, &depth, &local_choice, &branch)) {
    return MTP_RESP_INVALID_OBJECT_HANDLE;
  }
  const uint32_t local_index = local_choice - 1; // 0-based

  bool entry_is_dir = local_index != branch;
  int entry_name_len;
  uint16_t* entry_name_ptr = babel_entry_name(depth, local_index, branch, &entry_name_len);
  (void) entry_name_len; // mtp_container_add_string recomputes the length itself

  mtp_object_info_header_t obj_info_header = {
      .storage_id = SUPPORTED_STORAGE_ID,
      .object_format = entry_is_dir ? MTP_OBJ_FORMAT_ASSOCIATION : MTP_OBJ_FORMAT_TEXT,
      .protection_status = MTP_PROTECTION_STATUS_READ_ONLY,
      .object_compressed_size = -1,
      .thumb_format = MTP_OBJ_FORMAT_UNDEFINED,
      .thumb_compressed_size = 0,
      .thumb_pix_width = 0,
      .thumb_pix_height = 0,
      .image_pix_width = 0,
      .image_pix_height = 0,
      .image_bit_depth = 0,
      .parent_object = 0,
      .association_type = entry_is_dir ? MTP_ASSOCIATION_GENERIC_FOLDER : MTP_ASSOCIATION_UNDEFINED,
      .association_desc = 0,
      .sequence_number = 0};
  mtp_container_add_raw(io_container, &obj_info_header, sizeof(obj_info_header));
  mtp_container_add_string(io_container, entry_name_ptr);
  mtp_container_add_cstring(io_container, FS_FIXED_DATETIME);
  mtp_container_add_cstring(io_container, FS_FIXED_DATETIME);
  mtp_container_add_cstring(io_container, ""); // keywords, not used
  tud_mtp_data_send(io_container);
  // MTP_ESP_LOG("MtpImpl", "Reported %d: %s, size=%d", obj_handle, entry->name, stat_buf.st_size);

  return 0;
}

static int32_t fs_get_object(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  mtp_container_info_t* io_container = &cb_data->io_container;
  const uint32_t obj_handle = command->params[0];

  if (cb_data->phase == MTP_PHASE_COMMAND) {
    // If file contents is larger than CFG_TUD_MTP_EP_BUFSIZE, data may only partially is added here
    // the rest will be sent in tud_mtp_data_more_cb
    // Rants: TinyUSB MTP design is slightly annoying, it assumed your entire file to transfer is
    // always available in memory space, so he can write their own example code easier. But when you
    // work with reading file live from filesystem, it's awkward in the sense that if your file is
    // longer than the first packet you have to pretend to have added 100KB into the buffer (which
    // it can't hold, but the whole thing will pretend to have done that, records how many bytes
    // are actually transmitted, and next time in data phase you calculate the length of the rest)
    // We use a buffer that's large enough: our MTP packet is 512B, and the first time we transmit
    // something back, we need to send metadata back, so if we read a whole packet it's definitely
    // not gonna fit in the MTP packet's remaining space if our file is larger than the free space,
    // and when the file's smaller than that we're totally fine then.
    // Recompute output_file fresh from the handle table (obj_handle's own parent chain) before
    // reading - a pure function of obj_handle, with no dependency on whatever has happened on
    // other handles in between (background content indexing elsewhere in the tree, etc.).
    babel_block_t* block = babel_find_block(obj_handle);
    if (!block) {
      return MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    if (!babel_compute_index_for(block->parent_handle)) {
      return MTP_RESP_INVALID_OBJECT_HANDLE;
    }
    // output_file holds the running "index" (per the bijective big-number scheme), not the
    // final bytes directly - decode it into decoded_file now. This is non-destructive (works
    // off a scratch copy), so output_file/weight_file stay intact in case the user keeps
    // navigating deeper and reads another file.txt further down this same path.
    babel_decode_output_to_file();
    size_t file_size = decoded_file_len;
    // Pass file_size (not first_chunk) so add_raw records the full transfer length in header->len,
    // even though it only copies min(file_size, remaining_buf) bytes into the packet buffer.
    uint32_t bytes_queued = mtp_container_add_raw(io_container, decoded_file, file_size);
    (void) bytes_queued;
    tud_mtp_data_send(io_container);
  } else if (cb_data->phase == MTP_PHASE_DATA) {
    // continue sending remaining data: file contents offset is xferred byte minus header size
    const uint32_t offset = cb_data->total_xferred_bytes - sizeof(mtp_container_header_t);
    const uint32_t xact_len = tu_min32(decoded_file_len - offset, io_container->payload_bytes);
    uint8_t *write_ptr = io_container->payload;
    memcpy(write_ptr, decoded_file + offset, xact_len);
    tud_mtp_data_send(io_container);
  }

  return 0;

}

static int32_t fs_send_object_info(tud_mtp_cb_data_t* cb_data) {
  return MTP_RESP_OPERATION_NOT_SUPPORTED;

  // const mtp_container_command_t* command = cb_data->command_container;
  // mtp_container_info_t* io_container = &cb_data->io_container;
  // const uint32_t storage_id = command->params[0];
  // const uint32_t parent_handle = command->params[1]; // folder handle, 0xFFFFFFFF is root
  // (void) parent_handle;

  // MTP_ESP_LOG("MtpImpl", "fs_send_object_info");

  // if (!is_session_opened) {
  //   MTP_ESP_LOG("MtpImpl", "%s: session not open", __func__);
  //   return MTP_RESP_SESSION_NOT_OPEN;
  // }
  // if (storage_id != 0xFFFFFFFF && storage_id != SUPPORTED_STORAGE_ID) {
  //   MTP_ESP_LOG("MtpImpl", "%s: invalid storage ID %08X", __func__, storage_id);
  //   return MTP_RESP_INVALID_STORAGE_ID;
  // }

  // if (cb_data->phase == MTP_PHASE_COMMAND) {
  //   MTP_ESP_LOG("MtpImpl", "%s: command phase, receive first", __func__);
  //   tud_mtp_data_receive(io_container);
  // } else if (cb_data->phase == MTP_PHASE_DATA) {
  //   mtp_object_info_header_t* obj_info = (mtp_object_info_header_t*) io_container->payload;
  //   if (obj_info->storage_id != 0 && obj_info->storage_id != SUPPORTED_STORAGE_ID) {
  //     MTP_ESP_LOG("MtpImpl", "%s: data phase: invalid storage ID %08X", __func__, obj_info->storage_id);
  //     return MTP_RESP_INVALID_STORAGE_ID;
  //   }

  //   // 0xFFFFFFFF is root
  //   fs_handle_t parent_handle = (obj_info->parent_object == 0xFFFFFFFF ? 0 : obj_info->parent_object);

  //   if (parent_handle != 0) {
  //     fs_handletable_entry_t *entry;
  //     struct stat stat;
  //     int retval = fs_stat_handle(&handle_table, parent_handle, &stat, &entry);
  //     if (retval != 0 || (stat.st_mode & S_IFDIR) == 0) {
  //       ESP_LOGE("MtpImpl", "Invalid parent %X: stat retval %d, st_mode %X", parent_handle, retval, stat.st_mode);
  //       return MTP_RESP_INVALID_PARENT_OBJECT;
  //     }
  //   }

  //   if (obj_info->association_type == MTP_ASSOCIATION_UNDEFINED) {
  //     // Regular file
  //     if (!fs_can_create_file(&handle_table, obj_info->object_compressed_size)) {
  //       return MTP_RESP_STORE_FULL;
  //     }
  //     char filename[MTP_FILENAME_LENGTH];
  //     uint8_t* filename_buf = io_container->payload + sizeof(mtp_object_info_header_t);
  //     uint8_t filename_len = *filename_buf;
  //     size_t u8_filename_len = utf16_to_utf8((const utf16_t *)(filename_buf + 1),
  //                                          filename_len,
  //                                          (utf8_t *)filename,
  //                                          MTP_FILENAME_LENGTH);
  //     MTP_ESP_LOG("MtpImpl", "UTF16->UTF8 Conversion: expected UTF16 length: %d, output %d bytes, string:[%s]",
  //              filename_len, u8_filename_len, filename);
  //     fs_create_file(&handle_table, parent_handle, filename);
  //     // Here the current_file_size is used to hold the length-to-receive value till the send_object phase
  //     current_file_size = obj_info->object_compressed_size;
  //   } else if (obj_info->association_type == MTP_ASSOCIATION_GENERIC_FOLDER) {
  //     // Folder
  //     if (parent_handle != 0) {
  //       ESP_LOGE("MtpImpl", "Attempting to create folder in folder %d", parent_handle);
  //       return MTP_RESP_INVALID_PARENT_OBJECT;
  //     }
  //     char dir_path[100] = "/littlefs/";
  //     char *dir_name = dir_path + strlen(dir_path);
  //     uint8_t* filename_buf = io_container->payload + sizeof(mtp_object_info_header_t);
  //     uint8_t filename_len = *filename_buf;
  //     utf16_to_utf8((const utf16_t *)(filename_buf + 1),
  //                   filename_len,
  //                   (utf8_t *)dir_name,
  //                   sizeof(dir_path) - (dir_name - dir_path) - 1);
  //     mkdir(dir_path, 0777);
  //   } else {
  //     ESP_LOGE("MtpImpl", "Attempting to create unsupported association: %d", obj_info->association_type);
  //     return MTP_RESP_INVALID_PARAMETER;
  //   }
  //   // ignore date created/modified/keywords
  // }

  // return 0;
}

static int32_t fs_send_object(tud_mtp_cb_data_t* cb_data) {
  return MTP_RESP_OPERATION_NOT_SUPPORTED;
  // mtp_container_info_t* io_container = &cb_data->io_container;
  // if (current_file == NULL || current_handle == FS_INVALID_HANDLE) {
  //   return MTP_RESP_INVALID_OBJECT_HANDLE;
  // }

  // if (cb_data->phase == MTP_PHASE_COMMAND) {
  //   io_container->header->len += current_file_size;
  //   tud_mtp_data_receive(io_container);
  // } else if (cb_data->phase == MTP_PHASE_DATA) {
  //   // file contents offset is total xferred minus header size minus last received chunk
  //   // const uint32_t offset = cb_data->total_xferred_bytes - sizeof(mtp_container_header_t) - io_container->payload_bytes;
  //   // memcpy(f->data + offset, io_container->payload, io_container->payload_bytes);
  //   fwrite(io_container->payload, 1, io_container->payload_bytes, current_file);
  //   MTP_ESP_LOG("MtpImpl", "%s: data phase, written %d bytes to file", __func__, io_container->payload_bytes);
  //   if (cb_data->total_xferred_bytes - sizeof(mtp_container_header_t) < current_file_size) {
  //     MTP_ESP_LOG("MtpImpl", "%s: Starting new reception, %d bytes to go",
  //              __func__,
  //              current_file_size - cb_data->total_xferred_bytes + sizeof(mtp_container_header_t));
  //     MTP_ESP_LOG("MtpImpl", "%s: pcontainer->header->len = %d", __func__, io_container->header->len);
  //     tud_mtp_data_receive(io_container);
  //   } else {
  //     MTP_ESP_LOG("MtpImpl", "%s: File write completed, closing", __func__);
  //     fs_close_handle(current_handle, current_file);
  //   }
  // } else {
  //   ESP_LOGE("MtpImpl", "%s: Unknown phase %d", __func__, cb_data->phase);
  // }
  // MTP_ESP_LOG("MtpImpl", "%s: leaving", __func__);

  // return 0;
}

static int32_t fs_delete_object(tud_mtp_cb_data_t* cb_data) {
  return MTP_RESP_OPERATION_NOT_SUPPORTED;
  // const mtp_container_command_t* command = cb_data->command_container;
  // const uint32_t obj_handle = command->params[0];
  // const uint32_t obj_format = command->params[1]; // optional
  // (void) obj_format;

  // if (!is_session_opened) {
  //   return MTP_RESP_SESSION_NOT_OPEN;
  // }

  // char pathbuf[200];
  // if (!fs_path_from_handle(&handle_table, obj_handle, pathbuf, sizeof(pathbuf))) {
  //   return MTP_RESP_INVALID_OBJECT_HANDLE;
  // }

  // struct stat stat;
  // fs_handletable_entry_t *entry;
  // int retval = fs_stat_handle(&handle_table, obj_handle, &stat, &entry);
  // if (retval != 0) {
  //   ESP_LOGE("MtpImpl", "fs_delete_object failed to stat %s: %d", pathbuf, retval);
  //   return MTP_RESP_GENERAL_ERROR;
  // }

  // if (stat.st_mode & S_IFDIR) {
  //   // TODO: Is a directory, delete descendants
  //   return MTP_RESP_OPERATION_NOT_SUPPORTED;
  // } else {
  //   unlink(pathbuf);
  //   fs_delete_handle(&handle_table, obj_handle);
  // }

  // return MTP_RESP_OK;
}

int32_t fs_format_store(tud_mtp_cb_data_t* cb_data) {
  const mtp_container_command_t* command = cb_data->command_container;
  const uint32_t storage_id = command->params[0];

  if (!is_session_opened) {
    MTP_ESP_LOG("MtpImpl", "%s: session not open", __func__);
    return MTP_RESP_SESSION_NOT_OPEN;
  }
  if (storage_id != 0xFFFFFFFF && storage_id != SUPPORTED_STORAGE_ID) {
    MTP_ESP_LOG("MtpImpl", "%s: invalid storage ID %08X", __func__, storage_id);
    return MTP_RESP_INVALID_STORAGE_ID;
  }
  if (current_handle != FS_INVALID_HANDLE || current_file != NULL) {
    MTP_ESP_LOG("MtpImpl", "%s: storage %08X busy", __func__, storage_id);
    return MTP_RESP_DEVICE_BUSY;
  }
  // esp_err_t rc = esp_littlefs_format("littlefs");
  // if (rc == ESP_OK) {
  //   MTP_ESP_LOG("MtpImpl", "%s: format succeeded", __func__);

  //   handle_self_inc = 0;
  //   fs_handletable_regenerate(&handle_table);

    return MTP_RESP_OK;
  // } else {
  //   MTP_ESP_LOG("MtpImpl", "%s: format failed (rc = %d)", __func__, rc);

  //   return MTP_RESP_OPERATION_NOT_SUPPORTED;
  // }
}
