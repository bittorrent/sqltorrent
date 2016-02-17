/*
Copyright 2016 BitTorrent Inc

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "sqlite3ext.h"

SQLITE_EXTENSION_INIT1

#define BOOST_ASIO_SEPARATE_COMPILATION
#include <cassert>
#include <libtorrent/session.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/version.hpp>

namespace {

struct context
{
	context()
		: base(NULL), session(libtorrent::fingerprint("SL", 0, 1, 0, 0))
	{}

	sqlite3_vfs* base;
	libtorrent::session session;
};

struct torrent_vfs_file
{
	sqlite3_file base;
	libtorrent::session* session;
	libtorrent::torrent_handle torrent;
};

int vfs_close(sqlite3_file* file)
{
	torrent_vfs_file* f = (torrent_vfs_file*)file;
	f->session->remove_torrent(f->torrent);
	return SQLITE_OK;
}

int vfs_write(sqlite3_file* file, const void* buffer, int iAmt, sqlite3_int64 iOfst)
{
	assert(false);
	return SQLITE_OK;
}

int vfs_truncate(sqlite3_file* file, sqlite3_int64 size)
{
	assert(false);
	return SQLITE_ERROR;
}

int vfs_sync(sqlite3_file*, int flags)
{
	return SQLITE_OK;
}

int vfs_file_size(sqlite3_file* file, sqlite3_int64 *pSize)
{
	torrent_vfs_file* f = (torrent_vfs_file*)file;
	*pSize = f->torrent.torrent_file()->total_size();
	return SQLITE_OK;
}

int vfs_lock(sqlite3_file*, int)
{
	return SQLITE_OK;
}

int vfs_unlock(sqlite3_file*, int)
{
	return SQLITE_OK;
}

int vfs_check_reserved_lock(sqlite3_file*, int *pResOut)
{
	return SQLITE_OK;
}

int vfs_file_control(sqlite3_file*, int op, void *pArg)
{
	return SQLITE_OK;
}

int vfs_sector_size(sqlite3_file* file)
{
	torrent_vfs_file* f = (torrent_vfs_file*)file;
	return f->torrent.torrent_file()->piece_length();
}

int vfs_device_characteristics(sqlite3_file*)
{
	return SQLITE_IOCAP_IMMUTABLE;
}

int vfs_read(sqlite3_file* file, void* buffer, int iAmt, sqlite3_int64 iOfst)
{
	using namespace libtorrent;

	torrent_vfs_file* f = (torrent_vfs_file*)file;
	int const piece_size = vfs_sector_size(file);
	int piece_idx = int(iOfst / piece_size);
	int piece_offset = iOfst % piece_size;
	int residue = iAmt;
	std::uint8_t* b = (std::uint8_t*)buffer;

	do
	{
		f->torrent.set_piece_deadline(piece_idx, 0, torrent_handle::alert_when_available);

		for (;;)
		{
			alert const* a = f->session->wait_for_alert(seconds(10));
			if (!a) continue;

			if (a->type() != read_piece_alert::alert_type)
			{
				f->session->pop_alert();
				continue;
			}

			read_piece_alert const* pa = static_cast<read_piece_alert const*>(a);
			if (pa->piece != piece_idx)
			{
				f->session->pop_alert();
				continue;
			}


			assert(piece_offset < pa->size);
			assert(pa->size == piece_size);
			std::memcpy(b, pa->buffer.get() + piece_offset, (std::min<size_t>)(pa->size - piece_offset, residue));
			b += pa->size - piece_offset;
			residue -= pa->size - piece_offset;

			f->session->pop_alert();
			break;
		}

		piece_offset = 0;
	} while (residue > 0);

	return SQLITE_OK;
}

sqlite3_io_methods torrent_vfs_io_methods = {
	1,
	vfs_close,
	vfs_read,
	vfs_write,
	vfs_truncate,
	vfs_sync,
	vfs_file_size,
	vfs_lock,
	vfs_unlock,
	vfs_check_reserved_lock,
	vfs_file_control,
	vfs_sector_size,
	vfs_device_characteristics
};

int torrent_vfs_open(sqlite3_vfs* vfs, const char *zName, sqlite3_file* file, int flags, int *pOutFlags)
{
	using namespace libtorrent;

	assert(zName);
	context* ctx = (context*)vfs->pAppData;
	torrent_vfs_file* f = (torrent_vfs_file*)file;

	std::memset(f, 0, sizeof(torrent_vfs_file));
	f->base.pMethods = &torrent_vfs_io_methods;
	f->session = &ctx->session;

	ctx->session.set_alert_mask(alert::status_notification);

	{
		session_settings s = ctx->session.settings();
		s.enable_incoming_utp = false;
		s.enable_outgoing_utp = false;
		s.max_out_request_queue = 4;
		
		ctx->session.set_settings(s);
	}

	{
		pe_settings pes = ctx->session.get_pe_settings();
		pes.in_enc_policy = pes.out_enc_policy = pe_settings::disabled;
		ctx->session.set_pe_settings(pes);
	}

	add_torrent_params p;
	p.save_path = ".";
	error_code ec;
#if LIBTORRENT_VERSION_NUM < 10100
	p.ti = new torrent_info(zName, ec);
#else
	p.ti = boost::make_shared<torrent_info>(zName, ec);
#endif
	assert(!ec);
	try {
		f->torrent = ctx->session.add_torrent(p);
	} catch (libtorrent_exception e) {
		return SQLITE_CANTOPEN;
	}

	// I'm sad to say libtorrent has a bug where it incorrectly thinks a torrent is seeding
	// when it is in the checking_resume_data state. Wait here for the resume data to be checked
	// to avoid the bug.
	for (;;)
	{
		alert const* a = f->session->wait_for_alert(seconds(10));
		if (!a) continue;

		if (a->type() != torrent_checked_alert::alert_type)
		{
			f->session->pop_alert();
			continue;
		}

		break;
	}

	*pOutFlags |= SQLITE_OPEN_READONLY | SQLITE_OPEN_EXCLUSIVE;

	return SQLITE_OK;
}

int torrent_vfs_access(sqlite3_vfs* vfs, const char *zName, int flags, int *pResOut)
{
	context* ctx = (context*)vfs->pAppData;
	int rc = ctx->base->xAccess(ctx->base, zName, flags, pResOut);
	if (rc != SQLITE_OK) return rc;
	*pResOut &= ~SQLITE_ACCESS_READWRITE;
	return SQLITE_OK;
}

}

extern "C" {

#if defined(_WIN32) && !defined(SQLITE_CORE)
__declspec(dllexport)
#endif
int sqltorrent_init(int make_default)
{
	static context ctx;
	static sqlite3_vfs vfs;
	if (!ctx.base)
	{
		ctx.base = sqlite3_vfs_find(nullptr);
		vfs = *ctx.base;
		vfs.zName = "torrent";
		vfs.pAppData = &ctx;
		vfs.szOsFile = sizeof(torrent_vfs_file);
		vfs.xOpen = torrent_vfs_open;
		vfs.xAccess = torrent_vfs_access;
	}

	sqlite3_vfs_register(&vfs, make_default);

	return SQLITE_OK;
}

#if defined(_WIN32) && !defined(SQLITE_CORE)
__declspec(dllexport)
#endif
int sqlite3_sqltorrent_init(
	sqlite3 *db,
	char **pzErrMsg,
	const sqlite3_api_routines *pApi)
{
	int rc = SQLITE_OK;
	SQLITE_EXTENSION_INIT2(pApi);

	rc = sqltorrent_init(1);

	return rc;
}

}
