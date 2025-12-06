#include "datafile.h"

#include <string>
#include <list>
#include <set>
#include <cstdint>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <iomanip>
#include <algorithm>

#define dat_magic 0x33
#define init_magic 0xdb
#define next_magic(_magic) ((_magic + 1) % 256)

/**
 * Simple class for writing .cat files while keeping track of the magic encryption value.
 */
class cat_writer {
public:
	cat_writer(std::filesystem::path cat_path) : m_magic(init_magic), m_cat_path(cat_path) {}

	bool open() {
		m_catstream.open(m_cat_path);
		return (bool)m_catstream;
	}

	bool write(const std::string& data) {
		for (char c : data) {
			m_catstream.put(c ^ m_magic);
			m_magic = next_magic(m_magic);
		}

		m_catstream << std::flush;
		return (bool)m_catstream;
	}

private:
	uint8_t m_magic;
	std::filesystem::path m_cat_path;
	std::ofstream m_catstream;
};

// Read in a file
static std::vector<uint8_t> read_file_to_vector(const std::filesystem::path& file_path) {
	std::ifstream infile(file_path, std::ios::in | std::ios::binary);
	if (!infile) {
		return {};
	}

	// Get file size
	infile.seekg(0, std::ios::end);
	std::streamsize size = infile.tellg();
	infile.seekg(0, std::ios::beg);

	// Read into vector
	std::vector<uint8_t> buffer(size);
	infile.read((char*)buffer.data(), size);

	return buffer;
}

bool datafile::parse(const std::filesystem::path& catfilename) {
	std::vector<uint8_t> encrypted_cat = read_file_to_vector(catfilename);
	std::string datfilename;

	if (encrypted_cat.size() == 0) {
		return false;
	}
	m_unencrypted_cat.resize(encrypted_cat.size());

	// Decrypt the file and build the index
	uint32_t running_offset = 0;
	uint32_t lineptr = 0;
	uint32_t last_space = 0;
	bool first_line = true;
	uint8_t magic = init_magic;
	for (uint32_t idx = 0; idx < encrypted_cat.size(); ++idx) {
		const char line_end = 0x0a;
		m_unencrypted_cat[idx] = encrypted_cat[idx] ^ magic;
		magic = next_magic(magic);

		// Parse the line into an index entry
		if (m_unencrypted_cat[idx] == line_end) {
			if (first_line) {
				datfilename = std::string((char*)&m_unencrypted_cat[lineptr], idx - lineptr);
				first_line = false;
			} else if (last_space > lineptr && last_space < idx) {
				m_index.emplace_back(
					(char*)&m_unencrypted_cat[lineptr], last_space - lineptr, idx - lineptr, running_offset);
				running_offset += m_index.back().size;
			}
			last_space = lineptr = idx + 1;
		} else if (m_unencrypted_cat[idx] == ' ') {
			last_space = idx;
		}
	}

	m_catfile = catfilename.string();
	set_datafile(datfilename);

	return true;
}

std::optional<datafile::file_record> datafile::get_file_record(const std::string& filename, bool strict_match) const {
	for (const auto& entry : m_index) {
		if (strict_match ? (entry == filename) : entry.filename_match(filename)) {
			return file_record{entry.relpath, entry.offset, entry.size};
		}
	}

	return std::nullopt;
}

bool datafile::read_file_range(const file_record& record, size_t offset, size_t size, std::string& out) const {
	if (offset > record.size) {
		return false;
	}

	const size_t available = record.size - offset;
	const size_t read_len = std::min<size_t>(size, available);

	std::ifstream encoded_datafile(m_datfile, std::ios::in | std::ios::binary);
	if (!encoded_datafile) {
		std::ofstream("/tmp/x3fuse.log", std::ios::app)
			<< "read_file_range: unable to open dat file " << m_datfile << "\n";
		return false;
	}

	if (read_len == 0) {
		out.clear();
		return true;
	}

	encoded_datafile.seekg(record.offset + offset);
	std::string buffer;
	buffer.resize(read_len);
	encoded_datafile.read(buffer.data(), read_len);
	const auto got = static_cast<size_t>(encoded_datafile.gcount());
	if (got == 0 && read_len > 0) {
		std::ofstream("/tmp/x3fuse.log", std::ios::app)
			<< "read_file_range: short read from " << m_datfile << " at offset " << record.offset + offset
			<< " requested " << read_len << " record.size " << record.size << "\n";
		return false;
	}

	for (size_t i = 0; i < got; ++i) {
		buffer[i] ^= dat_magic;
	}

	buffer.resize(got);
	out.swap(buffer);
	return true;
}

bool write_file_to_dat(std::ostream& outfile, const std::filesystem::directory_entry& data) {
	std::ifstream infile(data.path(), std::ios::in | std::ios::binary);
	if (!infile) {
		return false;
	}

	char c;
	while (infile.get(c)) {
		outfile.put(c ^ dat_magic);
	}

	return (bool)outfile;
}

bool datafile::enumerate_directory(const std::filesystem::path& dir, std::set<std::filesystem::directory_entry>& fset) {
	std::error_code ec;
	std::filesystem::directory_iterator it(dir, ec);
	if (ec) {
		std::cerr << "Could not open directory " << dir << ": " << ec.message() << std::endl;
		return false;
	}

	for (auto const& curr_file : it) {
		if (curr_file.is_directory()) {
			if (!enumerate_directory(curr_file, fset)) {
				return false;
			}
		} else {
			fset.insert(curr_file);
		}
	}

	return true;
}

bool datafile::build(const std::filesystem::path& p, const std::filesystem::path& catfile) {
	// Check if the input path exists and is a directory
	if (!std::filesystem::exists(p)) {
		std::cerr << p << " does not exist\n";
		return false;
	}
	if (!std::filesystem::is_directory(p)) {
		std::cerr << p << " is not a directory\n";
		return false;
	}

	// Used to alphabetize the list (will be case sensitive, oh well)
	std::set<std::filesystem::directory_entry> fset;

	// Go ahead and try to open the output files, so we don't waste time if it fails
	std::filesystem::path datfile = catfile;
	datfile.replace_extension(".dat");
	cat_writer cwriter(catfile);
	std::ofstream datstream(datfile, std::ios::out | std::ios::binary | std::ios::trunc);

	if (!cwriter.open()) {
		std::cerr << "Could not open " << catfile << " for writing!\n";
		return false;
	}

	if (!datstream) {
		std::cerr << "Could not open " << datfile << " for writing!\n";
		return false;
	}

	// Enumerate (and flatten) files
	if (!enumerate_directory(p, fset)) {
		return false;
	}

	// Write the files
	uint32_t running_offset = 0;

	// The cat file starts with the filename of the corresponding dat file
	std::string datfile_header = datfile.filename().string() + "\n";
	if (!cwriter.write(datfile_header)) {
		std::cerr << "Error when writing to cat file\n";
		return false;
	}

	for (auto const& curr_file : fset) {
		// Write cat file
		std::stringstream cat_entry;
		// Compute path relative to the input directory
		std::filesystem::path rel_path = std::filesystem::relative(curr_file.path(), p);
		cat_entry << rel_path.generic_string() << " " << curr_file.file_size() << (char)0x0a;
		if (!cwriter.write(cat_entry.str())) {
			std::cerr << "Error when writing to cat file\n";
			return false;
		}

		// Write to dat file
		if (!write_file_to_dat(datstream, curr_file)) {
			std::cerr << "Error when writing to dat file\n";
			return false;
		}

		running_offset += curr_file.file_size();
	}

	return true;
}

std::string datafile::get_index_listing() const {
	std::stringstream ss;

	ss << m_catfile << "\n";
	for (const auto& entry : m_index) {
		ss << "\t" << std::setw(64) << std::left << entry.relpath << std::setw(12) << std::right << entry.size << "\n";
	}

	return ss.str();
}

bool datafile::decrypt_to_file(const std::filesystem::path& filename) const {
	std::ofstream outfile(filename, std::ios::out | std::ios::binary);

	if (!outfile) {
		return false;
	}

	outfile.write((const char*)(m_unencrypted_cat.data()), m_unencrypted_cat.size());
	outfile.close();
	return true;
}

bool datafile::extract_one_file(const std::string& filename,
                                const std::filesystem::path& outfilename,
                                bool strict_match) const {
	if (outfilename.empty()) {
		return false;
	}

	const uint32_t block_size = 4096;
	// Make sure the file is in our index
	const index_entry* file_entry = nullptr;
	for (const auto& entry : m_index) {
		if (strict_match) {
			if (entry == filename) {
				file_entry = &entry;
				break;
			}
		} else {
			if (entry.filename_match(filename)) {
				file_entry = &entry;
				break;
			}
		}
	}
	if (!file_entry) {
		std::cout << "Could not find file " << filename << " in catalog\n";
		return false;
	}

	// Create directory structure for output file if necessary
	std::filesystem::path outfile_path(outfilename);
	std::filesystem::path parent_dir = outfile_path.parent_path();
	if (!parent_dir.empty()) {
		std::error_code err;
		if (!std::filesystem::create_directories(parent_dir, err) && err.value() != 0) {
			std::cerr << "Failed to create directory " << parent_dir << ": " << err << std::endl;
			return false;
		}
	}

	// Open the input and output files
	std::ifstream encoded_datafile(m_datfile, std::ios::in | std::ios::binary);

	if (!encoded_datafile) {
		std::cerr << "Could not open data file " << m_datfile << std::endl;
		return false;
	}
	std::ofstream outfile(outfilename, std::ios::out | std::ios::binary);

	if (!outfile) {
		std::cerr << "Could not open output file " << outfilename << " for writing\n";
		return false;
	}

	// Read and decode the file
	encoded_datafile.seekg(file_entry->offset);
	uint8_t tmp[block_size];
	uint32_t len = file_entry->size;
	while ((len > 0) && encoded_datafile) {
		// Read one chunk
		uint32_t read_len = block_size;
		if (read_len > len)
			read_len = len;
		encoded_datafile.read((char*)tmp, read_len);
		read_len = encoded_datafile.gcount();
		len -= read_len;

		// Decode to output file
		for (uint32_t i = 0; i < read_len; ++i) {
			tmp[i] ^= 0x33;
		}
		outfile.write((char*)tmp, read_len);
	}

	if (len > 0 || !encoded_datafile) {
		std::cerr << "I/O error while decoding file\n";
		return false;
	}

	// Clean up
	encoded_datafile.close();
	outfile.close();
	return true;
}

bool datafile::extract(const std::filesystem::path& output_path) const {
	const std::filesystem::path p(output_path);

	for (const auto& entry : m_index) {
		std::filesystem::path entry_path = p / entry.relpath;
		std::filesystem::path out_path = entry_path.parent_path();

		std::error_code err;
		if (!std::filesystem::create_directories(out_path, err) && err.value() != 0) {
			std::cerr << "Failed to create directory " << out_path << ": " << err << std::endl;
			return false;
		}

		if (!extract_one_file(entry.relpath, entry_path)) {
			std::cerr << "Error when extracting " << entry.relpath << std::endl;
			return false;
		}
	}

	return true;
}

void datafile::set_datafile(const std::string& datafile) {
	std::filesystem::path cat_path(m_catfile);
	std::filesystem::path candidate(datafile);

	// Prefer an absolute path if given and it exists
	if (candidate.is_absolute() && std::filesystem::exists(candidate)) {
		m_datfile = candidate;
		return;
	}

	// Try resolving relative to the cat's directory
	std::filesystem::path cat_dir = cat_path.parent_path();
	if (!candidate.is_absolute()) {
		std::filesystem::path rel_candidate = cat_dir / candidate;
		if (std::filesystem::exists(rel_candidate)) {
			m_datfile = rel_candidate;
			return;
		}
	}

	// Fallback: swap extension on the cat file
	cat_path.replace_extension(".dat");
	if (std::filesystem::exists(cat_path)) {
		m_datfile = cat_path;
		return;
	}

	// Last resort: keep whatever was provided
	m_datfile = candidate.string();
}
