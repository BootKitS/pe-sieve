#pragma once

#include <windows.h>
#include <psapi.h>
#include <map>

#include <peconv.h>
#include "module_scan_report.h"
#include "workingset_scanner.h"
#include "../utils/process_util.h"

#define INVALID_OFFSET (-1)
#define PE_NOT_FOUND 0

namespace pesieve {

	bool is_valid_file_hdr(BYTE *loadedData, size_t loadedSize, BYTE *hdr_ptr, DWORD charact);
	bool is_valid_section(BYTE *loadedData, size_t loadedSize, BYTE *hdr_ptr, DWORD charact);

	class PeArtefacts {
	public:
		static const size_t JSON_LEVEL = 1;

		PeArtefacts() {
			regionStart = INVALID_OFFSET;
			peBaseOffset = INVALID_OFFSET;
			ntFileHdrsOffset = INVALID_OFFSET;
			secHdrsOffset = INVALID_OFFSET;
			secCount = 0;
			calculatedImgSize = 0;
			isMzPeFound = false;
			isDll = true;
			is64bit = false;
		}

		bool hasNtHdrs()
		{
			return (ntFileHdrsOffset != INVALID_OFFSET);
		}

		bool hasSectionHdrs()
		{
			return (secHdrsOffset != INVALID_OFFSET);
		}

		ULONGLONG peImageBase()
		{
			return this->peBaseOffset + this->regionStart;
		}

		ULONGLONG dropPeBase(const ULONGLONG offset_with_pe_base) const
		{
			if (peBaseOffset == INVALID_OFFSET || offset_with_pe_base == INVALID_OFFSET) {
				return INVALID_OFFSET;
			}
			if (offset_with_pe_base < peBaseOffset) {
				return INVALID_OFFSET;
			}
			return offset_with_pe_base - peBaseOffset;
		}

		const virtual bool fieldsToJSON(std::stringstream &outs, size_t level = JSON_LEVEL)
		{
			OUT_PADDED(outs, level, "\"pe_base_offset\" : ");
			outs << "\"" << std::hex << peBaseOffset << "\"";
			if (hasNtHdrs()) {
				outs << ",\n";
				OUT_PADDED(outs, level, "\"nt_file_hdr\" : ");
				outs << "\"" << std::hex << ntFileHdrsOffset << "\"";
			}
			outs << ",\n";
			OUT_PADDED(outs, level, "\"sections_hdrs\" : ");
			outs << "\"" << std::hex << secHdrsOffset << "\"";
			outs << ",\n";
			OUT_PADDED(outs, level, "\"sections_count\" : ");
			outs << std::dec << secCount;
			outs << ",\n";
#ifdef _DEBUG
			OUT_PADDED(outs, level, "\"calculated_image_size\" : ");
			outs << std::hex << this->calculatedImgSize;
			outs << ",\n";
#endif
			OUT_PADDED(outs, level, "\"is_dll\" : ");
			outs << std::dec << isDll;
			outs << ",\n";
			OUT_PADDED(outs, level, "\"is_64_bit\" : ");
			outs << std::dec << this->is64bit;
			return true;
		}

		const virtual bool toJSON(std::stringstream &outs, size_t level = JSON_LEVEL)
		{
			OUT_PADDED(outs, level, "\"pe_artefacts\" : {\n");
			fieldsToJSON(outs, level + 1);
			outs << "\n";
			OUT_PADDED(outs, level, "}");
			return true;
		}

		LONGLONG regionStart;
		size_t peBaseOffset; //offset from the regionStart (PE may not start at the first page of the region)
		size_t ntFileHdrsOffset; //offset from the regionStart
		size_t secHdrsOffset; //offset from the regionStart
		size_t secCount;
		size_t calculatedImgSize;
		bool isMzPeFound;
		bool isDll;
		bool is64bit;
	};

	class ArtefactScanReport : public WorkingSetScanReport
	{
	public:
		ArtefactScanReport(HANDLE processHandle, HMODULE _module, size_t _moduleSize, t_scan_status status, PeArtefacts &peArt)
			: WorkingSetScanReport(processHandle, _module, _moduleSize, status),
			artefacts(peArt),
			initialRegionSize(_moduleSize)
		{
			is_executable = true;
			protection = 0;
			has_pe = true;
			has_shellcode = false;

			size_t total_region_size = peArt.calculatedImgSize + peArt.peBaseOffset;
			if (total_region_size > this->moduleSize) {
				this->moduleSize = total_region_size;
			}
		}

		const virtual void fieldsToJSON(std::stringstream &outs, size_t level = JSON_LEVEL)
		{
			WorkingSetScanReport::fieldsToJSON(outs, level);
			outs << ",\n";
			artefacts.toJSON(outs, level);
		}

		const virtual bool toJSON(std::stringstream &outs, size_t level = JSON_LEVEL)
		{
			OUT_PADDED(outs, level, "\"workingset_scan\" : {\n");
			fieldsToJSON(outs, level + 1);
			outs << "\n";
			OUT_PADDED(outs, level, "}");
			return true;
		}

		PeArtefacts artefacts;
		size_t initialRegionSize;
	};

	class ArtefactScanner {
	public:

		static size_t calcImgSize(HANDLE processHandle, HMODULE modBaseAddr, BYTE* headerBuffer, size_t headerBufferSize, IMAGE_SECTION_HEADER *hdr_ptr = NULL);

		ArtefactScanner(HANDLE _procHndl, MemPageData &_memPageData, ProcessScanReport& _process_report)
			: processHandle(_procHndl), processReport(_process_report), isProcess64bit(false),
			memPage(_memPageData), prevMemPage(nullptr), artPagePtr(nullptr)
		{
			isProcess64bit = pesieve::util::is_process_64bit(this->processHandle);
		}

		virtual ~ArtefactScanner()
		{
			deletePrevPage();
		}

		virtual ArtefactScanReport* scanRemote();

	protected:
		class ArtefactsMapping
		{
		public:
			ArtefactsMapping(MemPageData &_memPage, bool _is64bit) :
				memPage(_memPage)
			{
				pe_image_base = PE_NOT_FOUND;
				dos_hdr = nullptr;
				nt_file_hdr = nullptr;
				sec_hdr = nullptr;
				isMzPeFound = false;
				sec_count = 0;
				is64bit = _is64bit;
			}

			bool foundAny()
			{
				if (sec_hdr || nt_file_hdr) {
					return true;
				}
				return false;
			}

			size_t getScore() const
			{
				size_t score = 0;
				if (sec_hdr) score += 3;
				if (nt_file_hdr) score += 2;
				if (dos_hdr) score++;
				return score;
			}

			bool operator < (const ArtefactsMapping& map2) const {
				return getScore() < map2.getScore();
			}

			ArtefactsMapping& operator = (const ArtefactsMapping& other) {
				this->pe_image_base = other.pe_image_base;
				this->dos_hdr = other.dos_hdr;
				this->nt_file_hdr = other.nt_file_hdr;
				this->sec_hdr = other.sec_hdr;
				this->sec_count = other.sec_count;
				this->isMzPeFound = other.isMzPeFound;
				this->is64bit = other.is64bit;
				return *this;
			}

			MemPageData &memPage;
			ULONGLONG pe_image_base;
			IMAGE_DOS_HEADER *dos_hdr;
			IMAGE_FILE_HEADER* nt_file_hdr;
			IMAGE_SECTION_HEADER* sec_hdr;
			size_t sec_count;
			bool isMzPeFound;
			bool is64bit;
		};

		void deletePrevPage()
		{
			delete this->prevMemPage;
			this->prevMemPage = nullptr;
			this->artPagePtr = nullptr;
		}

		bool hasShellcode(HMODULE region_start, size_t region_size, PeArtefacts &peArt);

		bool findMzPe(ArtefactsMapping &mapping, const size_t search_offset);
		bool setMzPe(ArtefactsMapping &mapping, IMAGE_DOS_HEADER* _dos_hdr);
		bool setSecHdr(ArtefactsMapping &mapping, IMAGE_SECTION_HEADER* _sec_hdr);
		bool setNtFileHdr(ArtefactScanner::ArtefactsMapping &aMap, IMAGE_FILE_HEADER* _nt_hdr);
		PeArtefacts *generateArtefacts(ArtefactsMapping &aMap);

		PeArtefacts* findArtefacts(MemPageData &memPage, size_t start_offset);
		PeArtefacts* findInPrevPages(ULONGLONG addr_start, ULONGLONG addr_stop);

		ULONGLONG _findMZoffset(MemPageData &memPage, LPVOID hdr_ptr);
		ULONGLONG calcPeBase(MemPageData &memPage, LPVOID hdr_ptr);
		size_t calcImageSize(MemPageData &memPage, IMAGE_SECTION_HEADER *hdr_ptr, ULONGLONG pe_image_base);

		IMAGE_FILE_HEADER* findNtFileHdr(MemPageData &memPage, const size_t start_offset, size_t stop_offset = INVALID_OFFSET);

		bool _validateSecRegions(MemPageData &memPage, LPVOID sec_hdr, size_t sec_count, ULONGLONG pe_image_base, bool is_virtual);
		bool _validateSecRegions(MemPageData &memPage, LPVOID sec_hdr, size_t sec_count);
		BYTE* _findSecByPatterns(BYTE *search_ptr, const size_t max_search_size);
		IMAGE_SECTION_HEADER* findSecByPatterns(MemPageData &memPageData, const size_t max_search_size, const size_t search_offset);

		IMAGE_DOS_HEADER* findMzPeHeader(MemPageData &memPage, const size_t search_offset);
		IMAGE_DOS_HEADER* _findDosHdrByPatterns(BYTE *search_ptr, const size_t max_search_size);
		IMAGE_DOS_HEADER* findDosHdrByPatterns(MemPageData &memPage, const size_t start_offset, size_t stop_offset = INVALID_OFFSET);

		HANDLE processHandle;
		MemPageData &memPage;
		MemPageData *prevMemPage;
		MemPageData *artPagePtr; //pointer to the page where the artefacts were found: either to memPage or to prevMemPage
		bool isProcess64bit;
		ProcessScanReport& processReport;
	};

}; //namespace pe-sieve
