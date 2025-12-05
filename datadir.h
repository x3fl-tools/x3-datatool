#pragma once

#include <cstdint>
#include <string>
#include <map>
#include <filesystem>

#include "datafile.h"

/**
 * Represents an entire directory of .cat / .dat files.
 */
class datadir {
public:
	datadir(const std::string& path);

	/**
	 * Add a file pair to the list of tracked files by .cat file path
	 */
	bool add(const std::string& datafile_path);
	/**
	 * Add a file pair that's already been parsed into a datafile to the list of tracked files
	 */
	bool add(datafile& file);

	/**
	 * Find which datafile has the definitive version of a file.
	 *
	 * strict_match controls whether just the filename is matched or the whole relative path.
	 * If strict_match is false and there are multiple files with the same name, search will
	 * return the first one encountered.
	 */
	datafile* search(const std::string& filename, bool strict_match = false);

	/**
	 * Extract the data to a target directory, following the standard precendece rules.
	 */
	bool extract(const std::filesystem::path& target_path);

	// Getters for testing
	size_t size() const { return m_dir_idx.size(); }
	bool has_id(uint32_t id) const { return m_dir_idx.find(id) != m_dir_idx.end(); }

	/**
	 * Build a map of file paths to the datafile that owns the highest-precedence copy.
	 */
	std::map<std::string, datafile*> build_precedence_map();

private:
	uint32_t get_id_from_filename(const std::string& filename) const;

	std::map<std::string, uint32_t> m_name_map;
	std::map<uint32_t, datafile> m_dir_idx;

	uint32_t m_largest_id;

	// Friend class for testing private methods
	friend class datadir_test_accessor;
};
