#if !defined(FUSE_USE_VERSION)
#if __has_include(<fuse3/fuse.h>)
#define FUSE_USE_VERSION 31
#else
#define FUSE_USE_VERSION 26
#endif
#endif

#if __has_include(<fuse3/fuse.h>)
#include <fuse3/fuse.h>
#elif __has_include(<fuse.h>)
#include <fuse.h>
#else
#error "libfuse headers not found. Install libfuse (v3 preferred)."
#endif

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <system_error>

#include "datadir.h"
#include "datafile.h"

struct virtual_file {
	enum class kind { Catalog, Loose };

	kind type;
	datafile* source;
	datafile::file_record record;
	std::filesystem::path real_path;
	uint64_t size;
};

struct mount_layer {
	std::filesystem::path root;
	datadir* dir;
	bool include_loose;
	std::set<std::string> skip_dirs; // names to skip when crawling loose files
};

class mount_index {
public:
	bool build(const std::vector<mount_layer>& layers) {
		m_files.clear();
		m_directories.clear();

		// Precedence: earlier layers are lower priority; later layers override.
		for (const auto& layer : layers) {
			if (layer.dir) {
				add_catalog_layer(*layer.dir);
			}
			if (layer.include_loose) {
				add_loose_files(layer.root, layer.skip_dirs);
			}
		}

		m_directories.emplace("", std::set<std::string>{});
		return !m_files.empty();
	}

	const virtual_file* get_file(const std::string& path) const {
		auto it = m_files.find(path);
		if (it == m_files.end()) {
			return nullptr;
		}
		return &it->second;
	}

	bool is_directory(const std::string& path) const {
		return path.empty() || m_directories.find(path) != m_directories.end();
	}

	bool list_directory(const std::string& path, std::vector<std::string>& entries) const {
		auto it = m_directories.find(path);
		if (it == m_directories.end()) {
			return false;
		}
		entries.assign(it->second.begin(), it->second.end());
		return true;
	}

private:
	static std::string strip_addon_prefix(const std::string& relpath) {
		std::filesystem::path p(relpath);
		if (p.empty()) {
			return "";
		}
		auto it = p.begin();
		if (it != p.end() && (*it == "addon2" || *it == "addon")) {
			std::filesystem::path stripped;
			for (++it; it != p.end(); ++it) {
				stripped /= *it;
			}
			return stripped.generic_string();
		}
		return p.generic_string();
	}

	void add_path(const std::string& relpath) {
		std::filesystem::path p(relpath);
		std::filesystem::path current;

		for (const auto& part : p) {
			std::string parent = current.empty() ? "" : current.generic_string();
			std::string child = part.generic_string();
			m_directories[parent].insert(child);
			current /= part;
		}
	}

	void add_catalog_layer(datadir& dir) {
		auto precedence = dir.build_precedence_map();

		for (const auto& [path, df] : precedence) {
			if (m_files.find(path) != m_files.end()) {
				continue;
			}

			auto record = df->get_file_record(path, true);
			if (!record) {
				continue;
			}

			std::string norm_path = strip_addon_prefix(path);
			if (norm_path.empty()) {
				continue;
			}

			m_files.erase(norm_path);
			m_files.emplace(norm_path, virtual_file{virtual_file::kind::Catalog, df, *record, {}, record->size});
			add_path(norm_path);
		}
	}

	void add_loose_files(const std::filesystem::path& root, const std::set<std::string>& skip_dirs) {
		std::filesystem::path root_abs = std::filesystem::absolute(root);
		std::error_code ec;
		std::filesystem::recursive_directory_iterator it(
			root_abs, std::filesystem::directory_options::skip_permission_denied, ec);
		if (ec) {
			return;
		}

		for (const auto& entry : it) {
			if (entry.is_directory()) {
				auto name = entry.path().filename().string();
				if (skip_dirs.find(name) != skip_dirs.end()) {
					it.disable_recursion_pending();
					continue;
				}
			}

			if (!entry.is_regular_file()) {
				continue;
			}

			auto ext = entry.path().extension().string();
			if (ext == ".cat" || ext == ".dat" || ext == ".CAT" || ext == ".DAT") {
				continue;
			}

			std::filesystem::path rel = std::filesystem::relative(entry.path(), root_abs, ec);
			if (ec) {
				continue;
			}

			std::string relstr = rel.generic_string();
			if (relstr.empty()) {
				continue;
			}

			relstr = strip_addon_prefix(relstr);
			if (relstr.empty()) {
				continue;
			}

			auto size = entry.file_size(ec);
			if (ec) {
				continue;
			}

			m_files.erase(relstr); // ensure loose file overrides lower-priority entry

			virtual_file vf;
			vf.type = virtual_file::kind::Loose;
			vf.source = nullptr;
			vf.record = {};
			vf.real_path = entry.path();
			vf.size = static_cast<uint64_t>(size);

			m_files.emplace(relstr, vf);
			add_path(relstr);
		}
	}

	std::unordered_map<std::string, virtual_file> m_files;
	std::unordered_map<std::string, std::set<std::string>> m_directories;
};

struct x3_context {
	datadir base_dir;
	std::unique_ptr<datadir> addon_dir;
	std::unique_ptr<datadir> addon2_dir;
	mount_index index;
	std::filesystem::path root_path;

	explicit x3_context(const std::string& root)
		: base_dir(std::filesystem::absolute(root).string()), root_path(std::filesystem::absolute(root)) {
		if (std::filesystem::exists(root_path / "addon")) {
			addon_dir = std::make_unique<datadir>((root_path / "addon").string());
		}
		if (std::filesystem::exists(root_path / "addon2")) {
			addon2_dir = std::make_unique<datadir>((root_path / "addon2").string());
		}
	}
};

static std::string normalize_path(const char* path) {
	if (!path) {
		return "";
	}
	std::string ret(path);
	if (!ret.empty() && ret[0] == '/') {
		ret.erase(0, 1);
	}
	while (!ret.empty() && ret.back() == '/') {
		ret.pop_back();
	}
	return ret;
}

static x3_context* get_ctx() {
	auto* ctx = fuse_get_context();
	return static_cast<x3_context*>(ctx->private_data);
}

static int x3_getattr(const char* path, struct stat* st, struct fuse_file_info*) {
	std::memset(st, 0, sizeof(struct stat));
	std::string relpath = normalize_path(path);
	x3_context* ctx = get_ctx();

	const auto* vf = ctx->index.get_file(relpath);
	if (vf) {
		st->st_mode = S_IFREG | 0444;
		st->st_nlink = 1;
		st->st_size = vf->size;
	} else if (ctx->index.is_directory(relpath)) {
		st->st_mode = S_IFDIR | 0555;
		st->st_nlink = 2;
	} else {
		return -ENOENT;
	}

	if (auto* fuse_ctx = fuse_get_context()) {
		st->st_uid = fuse_ctx->uid;
		st->st_gid = fuse_ctx->gid;
	}

	return 0;
}

#if FUSE_USE_VERSION >= 30
static int x3_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t, struct fuse_file_info*,
                      enum fuse_readdir_flags) {
#else
static int x3_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t, struct fuse_file_info*) {
#endif
	std::string relpath = normalize_path(path);
	x3_context* ctx = get_ctx();

	if (!ctx->index.is_directory(relpath)) {
		return -ENOENT;
	}

#if FUSE_USE_VERSION >= 30
	filler(buf, ".", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
	filler(buf, "..", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
#else
	filler(buf, ".", nullptr, 0);
	filler(buf, "..", nullptr, 0);
#endif

	std::vector<std::string> entries;
	if (!ctx->index.list_directory(relpath, entries)) {
		return -ENOENT;
	}

	for (const auto& entry : entries) {
#if FUSE_USE_VERSION >= 30
		filler(buf, entry.c_str(), nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
#else
		filler(buf, entry.c_str(), nullptr, 0);
#endif
	}

	return 0;
}

static int x3_open(const char* path, struct fuse_file_info* fi) {
	std::string relpath = normalize_path(path);
	x3_context* ctx = get_ctx();
	const auto* vf = ctx->index.get_file(relpath);

	if (!vf) {
		return -ENOENT;
	}

	if ((fi->flags & O_ACCMODE) != O_RDONLY) {
		return -EACCES;
	}

	fi->fh = reinterpret_cast<uint64_t>(vf);
	return 0;
}

static int x3_read(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi) {
	if (offset < 0) {
		return -EINVAL;
	}

	const virtual_file* vf = reinterpret_cast<virtual_file*>(fi->fh);
	x3_context* ctx = get_ctx();

	if (!vf) {
		std::string relpath = normalize_path(path);
		vf = ctx->index.get_file(relpath);
		if (!vf) {
			return -ENOENT;
		}
	}

	if (vf->type == virtual_file::kind::Catalog) {
		std::string data;
		if (!vf->source->read_file_range(vf->record, static_cast<size_t>(offset), size, data)) {
			std::cerr << "read_file_range failed for " << vf->record.relpath << " offset " << offset << " size "
			          << size << " dat " << vf->source->get_datfile_name() << std::endl;
			return -EIO;
		}

		std::memcpy(buf, data.data(), data.size());
		return static_cast<int>(data.size());
	}

	// Loose file
	std::ifstream infile(vf->real_path, std::ios::binary);
	if (!infile) {
		return -EIO;
	}

	if (static_cast<uint64_t>(offset) > vf->size) {
		return 0;
	}
	const uint64_t available = vf->size - static_cast<uint64_t>(offset);
	const size_t to_read = static_cast<size_t>(std::min<uint64_t>(available, size));

	infile.seekg(offset);
	infile.read(buf, to_read);
	if (!infile && !infile.eof()) {
		return -EIO;
	}

	return static_cast<int>(infile.gcount());
}

static fuse_operations build_ops() {
	fuse_operations ops{};
	ops.getattr = x3_getattr;
	ops.readdir = x3_readdir;
	ops.open = x3_open;
	ops.read = x3_read;
	return ops;
}

static fuse_operations x3_ops = build_ops();

static void usage(const char* prog) {
	std::cerr << "Usage: " << prog << " <data-dir> <mountpoint> [fuse options]\n"
	          << "Example: " << prog << " ./x3fl /mnt/x3 -f\n";
}

int main(int argc, char** argv) {
	if (argc < 3) {
		usage(argv[0]);
		return 1;
	}

	std::string data_root = argv[1];
	x3_context ctx(data_root);

	std::vector<mount_layer> layers;
	// Lowest priority first
	layers.push_back({ctx.root_path, &ctx.base_dir, true, {"addon", "addon2"}});
	if (ctx.addon_dir) {
		layers.push_back({ctx.root_path / "addon", ctx.addon_dir.get(), true, {}});
	}
	if (ctx.addon2_dir) {
		layers.push_back({ctx.root_path / "addon2", ctx.addon2_dir.get(), true, {}});
	}

	if (!ctx.index.build(layers)) {
		std::cerr << "Failed to build file index for " << data_root << "\n";
		return 1;
	}

	// Strip the data root from the argv list before passing to FUSE
	std::vector<char*> fuse_args;
	fuse_args.reserve(argc);
	fuse_args.push_back(argv[0]);
	for (int i = 2; i < argc; ++i) {
		fuse_args.push_back(argv[i]);
	}
	fuse_args.push_back(nullptr);

	return fuse_main(static_cast<int>(fuse_args.size() - 1), fuse_args.data(), &x3_ops, &ctx);
}
