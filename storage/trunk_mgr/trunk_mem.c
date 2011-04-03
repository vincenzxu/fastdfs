/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDFS may be copied only under the terms of the GNU General
* Public License V3, which may be found in the FastDFS source kit.
* Please visit the FastDFS Home Page http://www.csource.org/ for more detail.
**/

//trunk_mem.c

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "fdfs_define.h"
#include "chain.h"
#include "logger.h"
#include "sockopt.h"
#include "shared_func.h"
#include "pthread_func.h"
#include "tracker_types.h"
#include "tracker_proto.h"
#include "storage_global.h"
#include "storage_service.h"
#include "trunk_sync.h"
#include "trunk_mem.h"

int g_slot_min_size;
int g_trunk_file_size;

static int slot_max_size;
int g_store_path_mode = FDFS_STORE_PATH_ROUND_ROBIN;
int g_storage_reserved_mb = FDFS_DEF_STORAGE_RESERVED_MB;
int g_avg_storage_reserved_mb = FDFS_DEF_STORAGE_RESERVED_MB;
int g_store_path_index = 0;
int g_current_trunk_file_id = 0;
TrackerServerInfo g_trunk_server = {-1, 0};
bool g_if_use_trunk_file = false;
//bool g_if_trunker_self = false;
bool g_if_trunker_self = true;

static FDFSTrunkSlot *slots = NULL;
static FDFSTrunkSlot *slot_end = NULL;
static pthread_mutex_t trunk_file_lock;
static struct fast_mblock_man trunk_blocks_man;

static int trunk_create_next_file(FDFSTrunkFullInfo *pTrunkInfo);
static int trunk_add_node(FDFSTrunkNode *pNode);

static int trunk_restore_node(const FDFSTrunkFullInfo *pTrunkInfo);
static int trunk_delete_node(const FDFSTrunkFullInfo *pTrunkInfo);

static int trunk_init_slot(FDFSTrunkSlot *pTrunkSlot, const int bytes)
{
	int result;

	pTrunkSlot->size = bytes;
	pTrunkSlot->free_trunk_head = NULL;
	if ((result=init_pthread_lock(&(pTrunkSlot->lock))) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"init_pthread_lock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, STRERROR(result));
		return result;
	}

	return 0;
}

int storage_trunk_init()
{
	int result;
	int slot_count;
	int bytes;
	FDFSTrunkSlot *pTrunkSlot;

	memset(&g_trunk_server, 0, sizeof(g_trunk_server));
	g_trunk_server.sock = -1;

	slot_count = 1;
	slot_max_size = g_trunk_file_size / 2;
	bytes = g_slot_min_size;
	while (bytes < slot_max_size)
	{
		slot_count++;
		bytes *= 2;
	}
	slot_count++;

	slots = (FDFSTrunkSlot *)malloc(sizeof(FDFSTrunkSlot) * slot_count);
	if (slots == NULL)
	{
		result = errno != 0 ? errno : EIO;
		logError("file: "__FILE__", line: %d, " \
			"malloc %d bytes fail, " \
			"errno: %d, error info: %s", \
			__LINE__, (int)sizeof(FDFSTrunkSlot) * slot_count, \
			result, STRERROR(result));
		return result;
	}

	if ((result=trunk_init_slot(slots, 0)) != 0)
	{
		return result;
	}

	bytes = g_slot_min_size;
	slot_end = slots + slot_count;
	for (pTrunkSlot=slots+1; pTrunkSlot<slot_end; pTrunkSlot++)
	{
		if ((result=trunk_init_slot(pTrunkSlot, bytes)) != 0)
		{
			return result;
		}

		bytes *= 2;
	}
	(slot_end - 1)->size = slot_max_size;

	if ((result=init_pthread_lock(&trunk_file_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"init_pthread_lock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, STRERROR(result));
		return result;
	}

	if ((result=fast_mblock_init(&trunk_blocks_man, \
			sizeof(FDFSTrunkNode), 0)) != 0)
	{
		return result;
	}

	return 0;
}

int storage_trunk_destroy()
{
	return 0;
}

static char *trunk_info_dump(const FDFSTrunkFullInfo *pTrunkInfo, char *buff, \
				const int buff_size)
{
	snprintf(buff, buff_size, \
		"store_path_index=%d, " \
		"sub_path_high=%d, " \
		"sub_path_low=%d, " \
		"id=%d, offset=%d, size=%d, status=%d", \
		pTrunkInfo->path.store_path_index, \
		pTrunkInfo->path.sub_path_high, \
		pTrunkInfo->path.sub_path_low,  \
		pTrunkInfo->file.id, pTrunkInfo->file.offset, pTrunkInfo->file.size, \
		pTrunkInfo->status);

	return buff;
}

static FDFSTrunkSlot *trunk_get_slot(const int size)
{
	FDFSTrunkSlot *pSlot;

	for (pSlot=slots; pSlot<slot_end; pSlot++)
	{
		if (size <= pSlot->size)
		{
			return pSlot;
		}
	}

	return NULL;
}

int trunk_free_space(const FDFSTrunkFullInfo *pTrunkInfo)
{
	int result;
	struct fast_mblock_node *pMblockNode;
	FDFSTrunkNode *pTrunkNode;

	if (pTrunkInfo->file.size < g_slot_min_size)
	{
		return 0;
	}

	pMblockNode = fast_mblock_alloc(&trunk_blocks_man);
	if (pMblockNode == NULL)
	{
		result = errno != 0 ? errno : EIO;
		logError("file: "__FILE__", line: %d, " \
			"malloc %d bytes fail, " \
			"errno: %d, error info: %s", \
			__LINE__, (int)sizeof(FDFSTrunkNode), \
			result, STRERROR(result));
		return result;
	}

	pTrunkNode = (FDFSTrunkNode *)pMblockNode->data;
	memcpy(&pTrunkNode->trunk, pTrunkInfo, sizeof(FDFSTrunkFullInfo));

	pTrunkNode->pMblockNode = pMblockNode;
	pTrunkNode->trunk.status = FDFS_TRUNK_STATUS_FREE;

	return trunk_add_node(pTrunkNode);
}

static int trunk_add_node(FDFSTrunkNode *pNode)
{
	int result;
	FDFSTrunkSlot *pSlot;
	FDFSTrunkNode *pPrevious;
	FDFSTrunkNode *pCurrent;

	for (pSlot=slot_end-1; pSlot>=slots; pSlot--)
	{
		if (pNode->trunk.file.size >= pSlot->size)
		{
			break;
		}
	}

	pthread_mutex_lock(&pSlot->lock);

	pPrevious = NULL;
	pCurrent = pSlot->free_trunk_head;
	while (pCurrent != NULL && pNode->trunk.file.size > \
		pCurrent->trunk.file.size)
	{
		pPrevious = pCurrent;
		pCurrent = pCurrent->next;
	}

	pNode->next = pCurrent;
	if (pPrevious == NULL)
	{
		pSlot->free_trunk_head = pNode;
	}
	else
	{
		pPrevious->next = pNode;
	}

	result = trunk_binlog_write(time(NULL), TRUNK_OP_TYPE_ADD_SPACE, \
				&(pNode->trunk));
	pthread_mutex_unlock(&pSlot->lock);
	return result;
}

static int trunk_delete_node(const FDFSTrunkFullInfo *pTrunkInfo)
{
	int result;
	FDFSTrunkSlot *pSlot;
	FDFSTrunkNode *pPrevious;
	FDFSTrunkNode *pCurrent;

	for (pSlot=slot_end-1; pSlot>=slots; pSlot--)
	{
		if (pTrunkInfo->file.size >= pSlot->size)
		{
			break;
		}
	}
	
	pthread_mutex_lock(&pSlot->lock);
	pPrevious = NULL;
	pCurrent = pSlot->free_trunk_head;
	while (pCurrent != NULL && memcmp(&(pCurrent->trunk), pTrunkInfo, \
		sizeof(FDFSTrunkFullInfo)) != 0)
	{
		pPrevious = pCurrent;
		pCurrent = pCurrent->next;
	}

	if (pCurrent == NULL)
	{
		char buff[256];

		pthread_mutex_unlock(&pSlot->lock);
		logError("file: "__FILE__", line: %d, " \
			"can't find trunk entry: %s", \
			trunk_info_dump(pTrunkInfo, buff, sizeof(buff)));
		return ENOENT;
	}

	if (pPrevious == NULL)
	{
		pSlot->free_trunk_head = pCurrent->next;
	}
	else
	{
		pPrevious->next = pCurrent->next;
	}

	pthread_mutex_unlock(&pSlot->lock);
	result = trunk_binlog_write(time(NULL), TRUNK_OP_TYPE_DEL_SPACE, \
				&(pCurrent->trunk));
	fast_mblock_free(&trunk_blocks_man, pCurrent->pMblockNode);

	return result;
}

static int trunk_restore_node(const FDFSTrunkFullInfo *pTrunkInfo)
{
	int result;
	FDFSTrunkSlot *pSlot;
	FDFSTrunkNode *pCurrent;

	for (pSlot=slot_end-1; pSlot>=slots; pSlot--)
	{
		if (pTrunkInfo->file.size >= pSlot->size)
		{
			break;
		}
	}
	
	pthread_mutex_lock(&pSlot->lock);
	pCurrent = pSlot->free_trunk_head;
	while (pCurrent != NULL && memcmp(&(pCurrent->trunk), pTrunkInfo, \
		sizeof(FDFSTrunkFullInfo)) != 0)
	{
		pCurrent = pCurrent->next;
	}

	if (pCurrent == NULL)
	{
		char buff[256];

		pthread_mutex_unlock(&pSlot->lock);

		logError("file: "__FILE__", line: %d, " \
			"can't find trunk entry: %s", \
			trunk_info_dump(pTrunkInfo, buff, sizeof(buff)));
		return ENOENT;
	}

	pCurrent->trunk.status = FDFS_TRUNK_STATUS_FREE;
	pthread_mutex_unlock(&pSlot->lock);

	result = trunk_binlog_write(time(NULL), TRUNK_OP_TYPE_SET_SPACE_FREE, \
				pTrunkInfo);

	return result;
}

static int trunk_slit(FDFSTrunkNode *pNode, const int size)
{
	int result;
	struct fast_mblock_node *pMblockNode;
	FDFSTrunkNode *pTrunkNode;

	if (pNode->trunk.file.size - size < g_slot_min_size)
	{
		return 0;
	}

	pMblockNode = fast_mblock_alloc(&trunk_blocks_man);
	if (pMblockNode == NULL)
	{
		result = errno != 0 ? errno : EIO;
		logError("file: "__FILE__", line: %d, " \
			"malloc %d bytes fail, " \
			"errno: %d, error info: %s", \
			__LINE__, (int)sizeof(FDFSTrunkNode), \
			result, STRERROR(result));
		return result;
	}

	pTrunkNode = (FDFSTrunkNode *)pMblockNode->data;
	memcpy(pTrunkNode, pNode, sizeof(FDFSTrunkNode));

	pTrunkNode->pMblockNode = pMblockNode;
	pTrunkNode->trunk.file.offset = pNode->trunk.file.offset + size;
	pTrunkNode->trunk.file.size = pNode->trunk.file.size - size;
	pTrunkNode->trunk.status = FDFS_TRUNK_STATUS_FREE;

	result = trunk_add_node(pTrunkNode);
	if (result != 0)
	{
		return result;
	}

	pNode->trunk.file.size = size;
	return 0;
}

int trunk_alloc_space(const int size, FDFSTrunkFullInfo *pResult)
{
	FDFSTrunkSlot *pSlot;
	FDFSTrunkNode *pPreviousNode;
	FDFSTrunkNode *pTrunkNode;
	struct fast_mblock_node *pMblockNode;
	int result;

	pSlot = trunk_get_slot(size);
	if (pSlot == NULL)
	{
		return ENOENT;
	}

	while (1)
	{
		pthread_mutex_lock(&pSlot->lock);

		pPreviousNode = NULL;
		pTrunkNode = pSlot->free_trunk_head;
		while (pTrunkNode != NULL && \
			pTrunkNode->trunk.status == FDFS_TRUNK_STATUS_HOLD)
		{
			pPreviousNode = pTrunkNode;
			pTrunkNode = pTrunkNode->next;
		}

		if (pTrunkNode != NULL)
		{
			break;
		}

		pthread_mutex_unlock(&pSlot->lock);

		pSlot++;
		if (pSlot >= slot_end)
		{
			break;
		}
	}

	if (pTrunkNode != NULL)
	{
		if (pPreviousNode == NULL)
		{
			pSlot->free_trunk_head = pTrunkNode->next;
		}
		else
		{
			pPreviousNode->next = pTrunkNode->next;
		}

		pthread_mutex_unlock(&pSlot->lock);
	}
	else
	{
		pMblockNode = fast_mblock_alloc(&trunk_blocks_man);
		if (pMblockNode == NULL)
		{
			result = errno != 0 ? errno : EIO;
			logError("file: "__FILE__", line: %d, " \
				"malloc %d bytes fail, " \
				"errno: %d, error info: %s", \
				__LINE__, (int)sizeof(FDFSTrunkNode), \
				result, STRERROR(result));
			return result;
		}
		pTrunkNode = (FDFSTrunkNode *)pMblockNode->data;
		pTrunkNode->pMblockNode = pMblockNode;

		pTrunkNode->trunk.file.offset = 0;
		pTrunkNode->trunk.file.size = g_trunk_file_size;
		pTrunkNode->trunk.status = FDFS_TRUNK_STATUS_FREE;

		result = trunk_create_next_file(&(pTrunkNode->trunk));
		if (result != 0)
		{
			fast_mblock_free(&trunk_blocks_man, pMblockNode);
			return result;
		}
	}

	result = trunk_slit(pTrunkNode, size);
	if (result == 0)
	{
		pTrunkNode->trunk.status = FDFS_TRUNK_STATUS_HOLD;
		result = trunk_add_node(pTrunkNode);
	}
	else
	{
		trunk_add_node(pTrunkNode);
	}

	if (result == 0)
	{
		memcpy(pResult, &(pTrunkNode->trunk), sizeof(FDFSTrunkFullInfo));
	}

	return result;
}

int trunk_alloc_confirm(const FDFSTrunkFullInfo *pTrunkInfo, const int status)
{
	if (status == 0)
	{
		return trunk_delete_node(pTrunkInfo);
	}
	else
	{
		return trunk_restore_node(pTrunkInfo);
	}
}

static int trunk_create_next_file(FDFSTrunkFullInfo *pTrunkInfo)
{
	char buff[16];
	int i;
	int result;
	int filename_len;
	char filename[64];
	char full_filename[MAX_PATH_SIZE];
	int store_path_index;
	int sub_path_high;
	int sub_path_low;

	store_path_index = g_store_path_index;
	if (g_store_path_mode == FDFS_STORE_PATH_LOAD_BALANCE)
	{
		if (store_path_index < 0)
		{
			return ENOSPC;
		}
	}
	else
	{
		if (store_path_index >= g_path_count)
		{
			store_path_index = 0;
		}

		if (g_path_free_mbs[store_path_index] <= \
			g_avg_storage_reserved_mb)
		{
			for (i=0; i<g_path_count; i++)
			{
				if (g_path_free_mbs[i] > g_avg_storage_reserved_mb)
				{
					store_path_index = i;
					g_store_path_index = i;
					break;
				}
			}

			if (i == g_path_count)
			{
				return ENOSPC;
			}
		}

		g_store_path_index++;
		if (g_store_path_index >= g_path_count)
		{
			g_store_path_index = 0;
		}
	}

	pTrunkInfo->path.store_path_index = store_path_index;

	while (1)
	{
		pthread_mutex_lock(&trunk_file_lock);
		pTrunkInfo->file.id = ++g_current_trunk_file_id;
		pthread_mutex_unlock(&trunk_file_lock);

		int2buff(pTrunkInfo->file.id, buff);
		base64_encode_ex(&g_base64_context, buff, sizeof(int), \
				filename, &filename_len, false);

		storage_get_store_path(filename, filename_len, \
					&sub_path_high, &sub_path_low);

		pTrunkInfo->path.sub_path_high = sub_path_high;
		pTrunkInfo->path.sub_path_low = sub_path_low;

		trunk_get_full_filename(pTrunkInfo, full_filename, \
			sizeof(full_filename));
		if (!fileExists(full_filename))
		{
			break;
		}
	}

	if ((result=trunk_init_file(full_filename)) != 0)
	{
		return result;
	}

	return 0;
}

char *trunk_get_full_filename(const FDFSTrunkFullInfo *pTrunkInfo, \
		char *full_filename, const int buff_size)
{
	char filename[64];
	char *pStorePath;

	pStorePath = g_store_paths[pTrunkInfo->path.store_path_index];
	TRUNK_GET_FILENAME(pTrunkInfo->file.id, filename);

	snprintf(full_filename, buff_size, \
			"%s/data/"STORAGE_DATA_DIR_FORMAT"/" \
			STORAGE_DATA_DIR_FORMAT"/%s", \
			pStorePath, pTrunkInfo->path.sub_path_high, \
			pTrunkInfo->path.sub_path_low, filename);

	return full_filename;
}

void trunk_pack_header(const FDFSTrunkHeader *pTrunkHeader, char *buff)
{
	*(buff + FDFS_TRUNK_FILE_FILE_TYPE_OFFSET) = pTrunkHeader->file_type;
	int2buff(pTrunkHeader->alloc_size, \
		buff + FDFS_TRUNK_FILE_ALLOC_SIZE_OFFSET);
	int2buff(pTrunkHeader->file_size, \
		buff + FDFS_TRUNK_FILE_FILE_SIZE_OFFSET);
	int2buff(pTrunkHeader->crc32, \
		buff + FDFS_TRUNK_FILE_FILE_CRC32_OFFSET);
	int2buff(pTrunkHeader->mtime, \
		buff + FDFS_TRUNK_FILE_FILE_MTIME_OFFSET);
	memcpy(buff + FDFS_TRUNK_FILE_FILE_EXT_NAME_OFFSET, \
		pTrunkHeader->ext_name, FDFS_FILE_EXT_NAME_MAX_LEN);
}

void trunk_unpack_header(const char *buff, FDFSTrunkHeader *pTrunkHeader)
{
	pTrunkHeader->file_type = *(buff + FDFS_TRUNK_FILE_FILE_TYPE_OFFSET);
	pTrunkHeader->alloc_size = buff2int(
			buff + FDFS_TRUNK_FILE_ALLOC_SIZE_OFFSET);
	pTrunkHeader->file_size = buff2int(
			buff + FDFS_TRUNK_FILE_FILE_SIZE_OFFSET);
	pTrunkHeader->crc32 = buff2int(
			buff + FDFS_TRUNK_FILE_FILE_CRC32_OFFSET);
	pTrunkHeader->mtime = buff2int(
			buff + FDFS_TRUNK_FILE_FILE_MTIME_OFFSET);
	memcpy(pTrunkHeader->ext_name, buff + \
		FDFS_TRUNK_FILE_FILE_EXT_NAME_OFFSET, \
		FDFS_FILE_EXT_NAME_MAX_LEN);
	*(pTrunkHeader->ext_name + FDFS_FILE_EXT_NAME_MAX_LEN) = '\0';
}

int trunk_init_file_ex(const char *filename, const int64_t file_size)
{
	int fd;
	int result;
	/*
	int64_t remain_bytes;
	int write_bytes;
	char buff[256 * 1024];
	*/

	fd = open(filename, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0)
	{
		result = errno != 0 ? errno : EIO;
		logError("file: "__FILE__", line: %d, " \
			"open file %s fail, " \
			"errno: %d, error info: %s", \
			__LINE__, filename, \
			result, STRERROR(result));
		return result;
	}

	if (ftruncate(fd, file_size) == 0)
	{
		result = 0;
	}
	else
	{
		result = errno != 0 ? errno : EIO;
		logError("file: "__FILE__", line: %d, " \
			"ftruncate file %s fail, " \
			"errno: %d, error info: %s", \
			__LINE__, filename, \
			result, STRERROR(result));
	}

/*
	memset(buff, 0, sizeof(buff));
	remain_bytes = file_size;
	while (remain_bytes > 0)
	{
		write_bytes = remain_bytes > sizeof(buff) ? \
				sizeof(buff) : remain_bytes;
		if (write(fd, buff, write_bytes) != write_bytes)
		{
			result = errno != 0 ? errno : EIO;
			logError("file: "__FILE__", line: %d, " \
				"write file %s fail, " \
				"errno: %d, error info: %s", \
				__LINE__, filename, \
				result, STRERROR(result));
			close(fd);
			return result;
		}

		remain_bytes -= write_bytes;
	}
*/

	close(fd);
	return result;
}

int trunk_check_and_init_file_ex(const char *filename, const int64_t file_size)
{
	struct stat file_stat;
	int fd;
	int result;

	if (stat(filename, &file_stat) != 0)
	{
		result = errno != 0 ? errno : ENOENT;
		if (result != ENOENT)
		{
			logError("file: "__FILE__", line: %d, " \
				"stat file %s fail, " \
				"errno: %d, error info: %s", \
				__LINE__, filename, \
				result, STRERROR(result));
			return result;
		}

		return trunk_init_file_ex(filename, file_size);
	}

	if (file_stat.st_size >= file_size)
	{
		return 0;
	}

	logWarning("file: "__FILE__", line: %d, " \
		"file: %s, file size: "INT64_PRINTF_FORMAT \
		" < "INT64_PRINTF_FORMAT", should be resize", \
		__LINE__, filename, (int64_t)file_stat.st_size, file_size);

	fd = open(filename, O_WRONLY, 0644);
	if (fd < 0)
	{
		result = errno != 0 ? errno : EIO;
		logError("file: "__FILE__", line: %d, " \
			"open file %s fail, " \
			"errno: %d, error info: %s", \
			__LINE__, filename, \
			result, STRERROR(result));
		return result;
	}

	if (ftruncate(fd, file_size) == 0)
	{
		result = 0;
	}
	else
	{
		result = errno != 0 ? errno : EIO;
		logError("file: "__FILE__", line: %d, " \
			"ftruncate file %s fail, " \
			"errno: %d, error info: %s", \
			__LINE__, filename, \
			result, STRERROR(result));
	}

	close(fd);
	return result;
}

void trunk_file_info_encode(const FDFSTrunkFileInfo *pTrunkFile, char *str)
{
	char buff[sizeof(int) * 3];
	int len;

	int2buff(pTrunkFile->id, buff);
	int2buff(pTrunkFile->offset, buff + sizeof(int));
	int2buff(pTrunkFile->size, buff + sizeof(int) * 2);
	base64_encode_ex(&g_base64_context, buff, sizeof(buff), \
			str, &len, false);
}

void trunk_file_info_decode(const char *str, FDFSTrunkFileInfo *pTrunkFile)
{
	char buff[sizeof(int) * 3];
	int len;

	base64_decode_auto(&g_base64_context, str, FDFS_TRUNK_FILE_INFO_LEN, \
		buff, &len);

	pTrunkFile->id = buff2int(buff);
	pTrunkFile->offset = buff2int(buff + sizeof(int));
	pTrunkFile->size = buff2int(buff + sizeof(int) * 2);
}

bool trunk_check_size(const int64_t file_size)
{
	return file_size <= slot_max_size;
}

int trunk_file_stat_func(const int store_path_index, const char *true_filename,\
	const int filename_len, stat_func statfunc, \
	struct stat *pStat, FDFSTrunkFullInfo *pTrunkInfo)
{
	char full_filename[MAX_PATH_SIZE];
	char buff[128];
	char pack_buff[FDFS_TRUNK_FILE_HEADER_SIZE];
	int64_t file_size;
	int buff_len;
	int fd;
	int read_bytes;
	int result;
	FDFSTrunkHeader trunkHeader;

	pTrunkInfo->file.id = 0;
	if (filename_len <= FDFS_TRUE_FILE_PATH_LEN + \
		FDFS_FILENAME_BASE64_LENGTH + 1 + FDFS_FILE_EXT_NAME_MAX_LEN)
	{
		snprintf(full_filename, sizeof(full_filename), "%s/data/%s", \
			g_store_paths[store_path_index], true_filename);

		if (statfunc(full_filename, pStat) == 0)
		{
			return 0;
		}
		else
		{
			return errno != 0 ? errno : ENOENT;
		}
	}

	memset(buff, 0, sizeof(buff));
	base64_decode_auto(&g_base64_context, (char *)true_filename + \
		FDFS_TRUE_FILE_PATH_LEN, FDFS_FILENAME_BASE64_LENGTH, \
		buff, &buff_len);

	file_size = buff2long(buff + sizeof(int) * 2);
	if ((file_size & FDFS_TRUNK_FILE_SIZE) == 0)
	{
		snprintf(full_filename, sizeof(full_filename), "%s/data/%s", \
			g_store_paths[store_path_index], true_filename);

		if (statfunc(full_filename, pStat) == 0)
		{
			return 0;
		}
		else
		{
			return errno != 0 ? errno : ENOENT;
		}
	}

	if (filename_len <= FDFS_TRUE_FILE_PATH_LEN + \
		FDFS_FILENAME_BASE64_LENGTH + FDFS_TRUNK_FILE_INFO_LEN + \
			 FDFS_FILE_EXT_NAME_MAX_LEN)
	{
		return EINVAL;
	}

	trunk_file_info_decode(true_filename + FDFS_TRUE_FILE_PATH_LEN + \
		 FDFS_FILENAME_BASE64_LENGTH, &pTrunkInfo->file);

	trunkHeader.file_size = file_size & (~(FDFS_TRUNK_FILE_SIZE));
	trunkHeader.mtime = buff2int(buff + sizeof(int));
	trunkHeader.crc32 = buff2int(buff + sizeof(int) * 4);
	memcpy(trunkHeader.ext_name, true_filename + (filename_len - \
		(FDFS_FILE_EXT_NAME_MAX_LEN + 1)), \
		FDFS_FILE_EXT_NAME_MAX_LEN + 1); //include tailing '\0'
	trunkHeader.alloc_size = pTrunkInfo->file.size;
	trunkHeader.file_type = FDFS_TRUNK_FILE_TYPE_REGULAR;

	pTrunkInfo->path.store_path_index = store_path_index;
	pTrunkInfo->path.sub_path_high = strtol(true_filename, NULL, 16);
	pTrunkInfo->path.sub_path_low = strtol(true_filename + 3, NULL, 16);

	trunk_get_full_filename(pTrunkInfo, full_filename, \
				sizeof(full_filename));
	fd = open(full_filename, O_RDONLY);
	if (fd < 0)
	{
		return errno != 0 ? errno : EIO;
	}

	if (lseek(fd, pTrunkInfo->file.offset, SEEK_SET) < 0)
	{
		result = errno != 0 ? errno : EIO;
		close(fd);
		return result;
	}

	read_bytes = read(fd, buff, FDFS_TRUNK_FILE_HEADER_SIZE);
	result = errno;
	close(fd);
	if (read_bytes != FDFS_TRUNK_FILE_HEADER_SIZE)
	{
		return result != 0 ? result : EIO;
	}

	trunk_pack_header(&trunkHeader, pack_buff);
	if (memcmp(pack_buff+1, buff+1, FDFS_TRUNK_FILE_HEADER_SIZE - 1) != 0)
	{
		return ENOENT;
	}

	memset(pStat, 0, sizeof(struct stat));
	pStat->st_size = trunkHeader.file_size;
	pStat->st_mtime = trunkHeader.mtime;
	pStat->st_mode = S_IFREG;

	return 0;
}
