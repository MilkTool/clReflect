
#include "DatabaseBinarySerialiser.h"
#include "Database.h"
#include "DatabaseMetadata.h"

#include <vector>


namespace
{
	// 'crdb'
	const unsigned int FILE_HEADER = 0x62647263;
	const unsigned int FILE_VERSION = 1;


	template <typename TYPE>
	void Write(FILE* fp, const TYPE& val)
	{
		fwrite(&val, sizeof(val), 1, fp);
	}


	void Write(FILE* fp, const std::string& str)
	{
		int len = str.length();
		Write(fp, len);
		fwrite(str.c_str(), len, 1, fp);
	}


	template <typename TYPE, int SIZE>
	void CopyInteger(const crdb::Database&, char* dest, const char* source, int)
	{
		// Ensure the assumed size is the same as the machine size
		int assert_size_is_correct[sizeof(TYPE) == SIZE];
		(void)assert_size_is_correct;

		// Quick assign copy
		*(TYPE*)dest = *(TYPE*)source;
	}


	void CopyMemory(const crdb::Database&, char* dest, const char* source, int size)
	{
		memcpy(dest, source, size);
	}


	void CopyNameToHash(const crdb::Database& db, char* dest, const char* source, int)
	{
		// Strip the hash from the name
		crdb::Name& name = *(crdb::Name*)source;
		*(crdb::u32*)dest = name.hash;
	}


	void CopyHashToName(const crdb::Database& db, char* dest, const char* source, int)
	{
		// Write the name as looked up by the hash
		crdb::u32 hash = *(crdb::u32*)source;
		*(crdb::Name*)dest = db.GetName(hash);
	}


	template <void COPY_FUNC(const crdb::Database&, char*, const char*, int)>
	void CopyStridedData(const crdb::Database& db, char* dest, const char* source, int nb_entries, int dest_stride, int source_stride, int field_size)
	{
		// The compiler should be able to inline the call the COPY_FUNC for each entry
		for (int i = 0; i < nb_entries; i++)
		{
			COPY_FUNC(db, dest, source, field_size);
			dest += dest_stride;
			source += source_stride;
		}
	}


	void CopyBasicFields(const crdb::Database& db, char* dest, const char* source, int nb_entries, int dest_stride, int source_stride, int field_size)
	{
		// Use memcpy as a last resort - try at least to use some big machine-size types
		switch (field_size)
		{
		case (1): CopyStridedData< CopyInteger<bool, 1> >(db, dest, source, nb_entries, dest_stride, source_stride, field_size); break;
		case (2): CopyStridedData< CopyInteger<short, 2> >(db, dest, source, nb_entries, dest_stride, source_stride, field_size); break;
		case (4): CopyStridedData< CopyInteger<int, 4> >(db, dest, source, nb_entries, dest_stride, source_stride, field_size); break;
		case (8): CopyStridedData< CopyInteger<__int64, 8> >(db, dest, source, nb_entries, dest_stride, source_stride, field_size); break;
		default: CopyStridedData< CopyMemory >(db, dest, source, nb_entries, dest_stride, source_stride, field_size); break;
		}
	}


	template <typename TYPE>
	void PackTable(const crdb::Database& db, const std::vector<TYPE>& table, const crdb::meta::DatabaseType& type, char* output)
	{
		// Walk up through the inheritance hierarhcy
		for (const crdb::meta::DatabaseType* cur_type = &type; cur_type; cur_type = cur_type->base_type)
		{
			// Pack a field at a time
			for (size_t i = 0; i < cur_type->fields.size(); i++)
			{
				const crdb::meta::DatabaseField& field = cur_type->fields[i];

				// Start at the offset from the field within the first object
				char* dest = output + field.packed_offset;
				const char* source = (char*)&table.front() + field.offset;

				// Perform strided copies depending on field type - pass information about the root type
				switch (field.type)
				{
				case (crdb::meta::FIELD_TYPE_BASIC): CopyBasicFields(db, dest, source, table.size(), type.packed_size, type.size, field.size); break;
				case (crdb::meta::FIELD_TYPE_NAME): CopyStridedData<CopyNameToHash>(db, dest, source, table.size(), type.packed_size, type.size, field.size); break;
				}
			}
		}
	}


	template <typename TYPE>
	void CopyPrimitiveStoreToTable(const crdb::PrimitiveStore<TYPE>& store, std::vector<TYPE>& table)
	{
		int dest_index = 0;

		// Make a local copy of all entries in the table
		table.resize(store.size());
		for (crdb::PrimitiveStore<TYPE>::const_iterator i = store.begin(); i != store.end(); ++i)
		{
			table[dest_index++] = i->second;
		}
	}


	template <typename TYPE>
	void WriteTable(FILE* fp, const crdb::Database& db, const crdb::meta::DatabaseTypes& dbtypes, bool named)
	{
		// Generate a memory-contiguous table
		std::vector<TYPE> table;
		CopyPrimitiveStoreToTable(db.GetPrimitiveStore<TYPE>(named), table);

		// Record the table size
		int table_size = table.size();
		Write(fp, table_size);

		if (table_size)
		{
			// Allocate enough memory to store the table in packed binary format
			const crdb::meta::DatabaseType& type = dbtypes.GetType<TYPE>();
			int packed_size = table_size * type.packed_size;
			char* data = new char[packed_size];

			// Binary pack the table
			PackTable(db, table, type, data);

			// Write to file and cleanup
			fwrite(data, packed_size, 1, fp);
			delete [] data;
		}
	}


	void WriteNameTable(FILE* fp, const crdb::Database& db)
	{
		// Write the table header
		int nb_names = db.m_Names.size();
		Write(fp, nb_names);

		// Write each name
		for (crdb::NameMap::const_iterator i = db.m_Names.begin(); i != db.m_Names.end(); ++i)
		{
			Write(fp, i->second.hash);
			Write(fp, i->second.text);
		}
	}
}


void crdb::WriteBinaryDatabase(const char* filename, const Database& db)
{
	FILE* fp = fopen(filename, "wb");

	// Write the header
	Write(fp, FILE_HEADER);
	Write(fp, FILE_VERSION);

	// Write each table with explicit ordering
	crdb::meta::DatabaseTypes dbtypes;
	WriteNameTable(fp, db);
	WriteTable<crdb::Namespace>(fp, db, dbtypes, true);
	WriteTable<crdb::Type>(fp, db, dbtypes, true);
	WriteTable<crdb::Class>(fp, db, dbtypes, true);
	WriteTable<crdb::Enum>(fp, db, dbtypes, true);
	WriteTable<crdb::EnumConstant>(fp, db, dbtypes, true);
	WriteTable<crdb::Function>(fp, db, dbtypes, true);
	WriteTable<crdb::Field>(fp, db, dbtypes, true);
	WriteTable<crdb::Field>(fp, db, dbtypes, false);

	fclose(fp);
}


namespace
{
	template <typename TYPE>
	TYPE Read(FILE* fp)
	{
		TYPE val;
		fread(&val, sizeof(val), 1, fp);
		return val;
	}


	template <> std::string Read<std::string>(FILE* fp)
	{
		char data[1024];

		// Clamp length to the available buffer size
		int len = Read<int>(fp);
		len = len > sizeof(data) - 1 ? sizeof(data) - 1 : len;

		fread(data, len, 1, fp);
		data[len] = 0;
		return data;
	}


	void ReadNameTable(FILE* fp, crdb::Database& db)
	{
		// Read the table header
		int nb_names = Read<int>(fp);

		// Read and populate each name
		for (int i = 0; i < nb_names; i++)
		{
			crdb::u32 hash = Read<crdb::u32>(fp);
			std::string str = Read<std::string>(fp);
			db.m_Names[hash] = crdb::Name(hash, str);
		}
	}


	template <typename TYPE>
	void UnpackTable(const crdb::Database& db, std::vector<TYPE>& table, const crdb::meta::DatabaseType& type, const char* input)
	{
		// Walk up through the inheritance hierarhcy
		for (const crdb::meta::DatabaseType* cur_type = &type; cur_type; cur_type = cur_type->base_type)
		{
			// Unpack a field at a time
			for (size_t i = 0; i < cur_type->fields.size(); i++)
			{
				const crdb::meta::DatabaseField& field = cur_type->fields[i];

				// Start at the offset from the field within the first object
				char* dest = (char*)&table.front() + field.offset;
				const char* source = input + field.packed_offset;

				// Perform strided copies depending on field type - pass information about the root type
				switch (field.type)
				{
				case (crdb::meta::FIELD_TYPE_BASIC): CopyBasicFields(db, dest, source, table.size(), type.size, type.packed_size, field.size); break;
				case (crdb::meta::FIELD_TYPE_NAME): CopyStridedData<CopyHashToName>(db, dest, source, table.size(), type.size, type.packed_size, field.size); break;
				}
			}
		}
	}


	template <typename TYPE>
	void ReadTable(FILE* fp, crdb::Database& db, const crdb::meta::DatabaseTypes& dbtypes)
	{
		// Create a big enough table dest
		int table_size = Read<int>(fp);
		std::vector<TYPE> table(table_size);

		if (table_size)
		{
			// Allocate enough memory to store the entire table in packed binary format and read it from the file
			const crdb::meta::DatabaseType& type = dbtypes.GetType<TYPE>();
			int packed_size = table_size * type.packed_size;
			char* data = new char[packed_size];
			fread(data, packed_size, 1, fp);

			// Unpack the binary table
			UnpackTable(db, table, type, data);
			delete [] data;

			// Add to the database
			for (size_t i = 0; i < table.size(); i++)
			{
				db.AddPrimitive(table[i]);
			}
		}
	}
}


bool crdb::ReadBinaryDatabase(const char* filename, Database& db)
{
	if (!IsBinaryDatabase(filename))
	{
		return false;
	}

	FILE* fp = fopen(filename, "rb");

	// Discard the header
	Read<unsigned int>(fp);
	Read<unsigned int>(fp);

	// Read each table with explicit ordering
	crdb::meta::DatabaseTypes dbtypes;
	ReadNameTable(fp, db);
	ReadTable<crdb::Namespace>(fp, db, dbtypes);
	ReadTable<crdb::Type>(fp, db, dbtypes);
	ReadTable<crdb::Class>(fp, db, dbtypes);
	ReadTable<crdb::Enum>(fp, db, dbtypes);
	ReadTable<crdb::EnumConstant>(fp, db, dbtypes);
	ReadTable<crdb::Function>(fp, db, dbtypes);
	ReadTable<crdb::Field>(fp, db, dbtypes);
	ReadTable<crdb::Field>(fp, db, dbtypes);

	fclose(fp);

	return true;
}


bool crdb::IsBinaryDatabase(const char* filename)
{
	// Not a database if the file can't be found
	FILE* fp = fopen(filename, "rb");
	if (fp == 0)
	{
		fclose(fp);
	}

	// Read the header and check it
	unsigned int header = Read<unsigned int>(fp);
	unsigned int version = Read<unsigned int>(fp);
	bool is_binary_db = header == FILE_HEADER && version == FILE_VERSION;

	fclose(fp);
	return is_binary_db;
}