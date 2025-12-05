#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <filesystem>

#include "datadir.h"


datadir::datadir(const std::string& path) : m_largest_id(0) {
	// Iterate through all files in the path, looking for cat files
	std::filesystem::path dir_path = std::filesystem::absolute(std::filesystem::path(path));

	// Check if directory exists
	if (!std::filesystem::exists(dir_path) || !std::filesystem::is_directory(dir_path)) {
		return;
	}

	// Iterate through directory entries
	for (const auto& entry : std::filesystem::directory_iterator(dir_path)) {
		if (entry.is_regular_file()) {
			std::string filename = entry.path().string();

			// Check if it's a .cat file
			if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".cat") {
				// Try to add the datafile
				add(filename);
			}
		}
	}
}

bool datadir::add(const std::string& datafile_path) {
	// Get the ID from the filename
	uint32_t id;
	std::filesystem::path full_path = std::filesystem::absolute(std::filesystem::path(datafile_path));

	try {
		id = get_id_from_filename(full_path.string());
	} catch (const std::exception&) {
		// Failed to parse ID from filename
		return false;
	}

	// If the ID already exists, fail the add
	if (m_dir_idx.contains(id)) {
		return false;
	}

	// Store the mappings
	m_name_map[full_path.string()] = id;
	m_dir_idx.emplace(id, full_path.string());

	if (id > m_largest_id) {
		m_largest_id = id;
	}

	return true;
}

bool datadir::add(datafile& file) {
	// Get the filename from the datafile
	std::string filename = std::filesystem::absolute(file.get_catfile_name()).string();
	if (filename.empty()) {
		return false;
	}
	uint32_t id = get_id_from_filename(filename);

	if (m_dir_idx.contains(id)) {
		return false;
	}

	// Store the filename mapping
	m_name_map[filename] = id;

	// Create a datafile in-place in the map
	m_dir_idx.emplace(id, filename);

	if (id > m_largest_id) {
		m_largest_id = id;
	}

	return true;
}

datafile* datadir::search(const std::string& filename, bool strict_match) {
	// Search from highest ID down to 0
	// Start at m_largest_id and work down
	for (uint32_t curr = m_largest_id; curr > 0; --curr) {
		// Check if this ID exists in our map
		auto it = m_dir_idx.find(curr);
		if (it != m_dir_idx.end()) {
			// Check if this datafile contains the file
			datafile& df = it->second;
			if (df.has_file(filename, strict_match)) {
				return &df;
			}
		}
	}

	// File not found in any datafile
	return nullptr;
}

std::map<std::string, datafile*> datadir::build_precedence_map() {
	std::map<std::string, datafile*> file_precedence;

	for (uint32_t curr = m_largest_id; curr > 0; --curr) {
		auto it = m_dir_idx.find(curr);
		if (it != m_dir_idx.end()) {
			datafile& df = it->second;
			auto file_list = df.get_file_list();
			for (const auto& filepath : file_list) {
				if (file_precedence.find(filepath) == file_precedence.end()) {
					file_precedence[filepath] = &df;
				}
			}
		}
	}

	return file_precedence;
}

bool datadir::extract(const std::filesystem::path& target_path) {
	if (!std::filesystem::exists(target_path) || !std::filesystem::is_directory(target_path)) {
		std::cerr << target_path << " does not exist or is not a directory\n";
		return false;
	}

	// Build a map of file paths to the datafile with highest precedence
	// Iterate from highest ID to lowest, so later iterations overwrite earlier ones
	std::map<std::string, datafile*> file_precedence;

	for (uint32_t curr = m_largest_id; curr > 0; --curr) {
		auto it = m_dir_idx.find(curr);
		if (it != m_dir_idx.end()) {
			datafile& df = it->second;
			auto file_list = df.get_file_list();
			for (const auto& filepath : file_list) {
				// Only add if not already present (higher ID has precedence)
				if (file_precedence.find(filepath) == file_precedence.end()) {
					file_precedence[filepath] = &df;
				}
			}
		}
	}

	// Extract each file from the correct datafile
	for (const auto& [filepath, df] : file_precedence) {
		std::filesystem::path output_file = target_path / filepath;

		if (!df->extract_one_file(filepath, output_file, true)) {
			std::cerr << "Failed to extract " << filepath << " from " << df->get_catfile_name() << "\n";
			return false;
		}
	}

	return true;
}

uint32_t datadir::get_id_from_filename(const std::string& filename) const {
	// The normal case is that the file is named ##.cat, so we can just use that number
	std::filesystem::path p(filename);

	// Get just the filename without directory path
	std::string basename = p.filename().string();

	// Remove the .cat extension if present
	if (basename.size() > 4 && basename.substr(basename.size() - 4) == ".cat") {
		basename = basename.substr(0, basename.size() - 4);
	}

	// Convert to number
	// If conversion fails, stoul will throw, but caller should ensure valid filenames
	return std::stoul(basename);
}
