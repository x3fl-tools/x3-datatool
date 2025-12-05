#pragma once

#include <string>
#include <list>
#include <set>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <vector>
#include <optional>

/**
 * Represents a single cat / dat pair.
 *
 * The .cat file is the index (catalog) of the contents of the .dat file. This
 * class manages the pair as a unit, providing functions to inspect, decode,
 * and build these data files and corresponding catalogs.
 */
class datafile {
public:
	datafile() {}
	datafile(const std::filesystem::path& catfilename) {
		if (parse(catfilename)) {
			m_catfile = catfilename.string();
		}
	}

	/**
	 * Given a .cat file, decrypt it and store the file list.
	 */
	bool parse(const std::filesystem::path& catfilename);

	/**
	 * Build a .cat and .dat file from a directory.
	 */
	bool build(const std::filesystem::path& p, const std::filesystem::path& catfile);

	/**
	 * Write a nicely-formatted listing for the catalog file to a string.
	 */
	std::string get_index_listing() const;

	/**
	 * Write out a decrypted version of the catalog file.
	 */
	bool decrypt_to_file(const std::filesystem::path& filename) const;

	/**
	 * Decrypt a single file from the data file.
	 */
	bool extract_one_file(const std::string& filename,
	                      const std::filesystem::path& outfilename,
	                      bool strict_match = false) const;

	/**
	 * Decrypt every file in the data file into a filesystem hierarchy.
	 */
	bool extract(const std::filesystem::path& output_path) const;

	/**
	 * Gets the name of the .dat file associated with this data pair.
	 */
	const std::string& get_datfile_name() const { return m_datfile; }

	/**
	 * Gets the name of the .cat file for this data pair.
	 */
	const std::string& get_catfile_name() const { return m_catfile; }

	/**
	 * Get a list of file paths inside that data file.
	 */
	std::list<std::string> get_file_list() const {
		std::list<std::string> ret;

		for (const auto& entry : m_index) {
			ret.push_back(entry.relpath);
		}

		return ret;
	}

	/**
	 * Represents one catalog entry with metadata that callers can safely use.
	 */
	struct file_record {
		std::string relpath;
		uint32_t offset;
		uint32_t size;
	};

	/**
	 * Find a file by path (or filename) and return metadata needed to read it.
	 */
	std::optional<file_record> get_file_record(const std::string& filename, bool strict_match = false) const;

	/**
	 * Read a portion of a file into a string, decoding on the fly.
	 */
	bool read_file_range(const file_record& record, size_t offset, size_t size, std::string& out) const;

	/**
	 * Check if this datafile contains a file with the given name.
	 */
	bool has_file(const std::string& filename, bool strict_match = false) const {
		for (const auto& entry : m_index) {
			if (strict_match) {
				if (entry == filename) {
					return true;
				}
			} else {
				if (entry.filename_match(filename)) {
					return true;
				}
			}
		}
		return false;
	}

private:
	/**
	 * Represents one entry in the .cat file
	 */
	struct index_entry {
		std::string relpath;
		uint32_t offset;
		uint32_t size;

		/**
		 * Read one index entry given a line in the index file.
		 * An entry looks like:
		 * <filename> <size>
		 */
		index_entry(const char* line, uint32_t delim_offset, uint32_t len, uint32_t file_offset)
			: relpath(line, delim_offset), offset(file_offset) {
			std::string sizestr(&line[delim_offset + 1], len - delim_offset);
			std::stringstream ss(sizestr);
			ss >> size;
		}

		bool operator==(const std::string& str) const { return relpath == str; }

		bool filename_match(const std::string& filename) const {
			std::filesystem::path entry_path(relpath);
			std::filesystem::path target_path(filename);
			return entry_path.filename() == target_path.filename();
		}
	};

	void set_datafile(const std::string& datafile);

	bool enumerate_directory(const std::filesystem::path& dir, std::set<std::filesystem::directory_entry>& fset);

	std::string m_catfile;
	std::string m_datfile;

	std::list<index_entry> m_index;
	std::vector<uint8_t> m_unencrypted_cat;
};
