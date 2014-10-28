#include "SequenceProvider.h"
//#include "NGM.h"
#include "IConfig.h"
#include "Log.h"

#include <vector>
#include <map>
#include <string>
#include <cmath>

#include <string.h>
#include <limits.h>

#include "Debug.h"

#include <iostream>

#undef module_name
#define module_name "SEQPROV"

_SequenceProvider * _SequenceProvider::pInstance = 0;

_SequenceProvider & _SequenceProvider::Instance() {
	if (pInstance == 0)
		pInstance = new _SequenceProvider();

	return *pInstance;
}

//static int const maxRefCount = 32768;
static int const maxRefCount = 2147483647;
static uint const refEncCookie = 0x74656;

//const size_t _SequenceProvider::MAX_REF_NAME_LENGTH = 150;

//bool checkChar(char c) {
//	return (c == 'A') | (c == 'C') | (c == 'T') | (c == 'G');
//}
//
//// checked die sequence seq auf ACGT, liefert das erste auftreten eines von diesen
//// buchstaben unterschiedlichen zeichens ...4-basen-worte (32bit) verwenden?
//int checkSequence(char * seq, uint len) {
//	for (uint n = 0; n < len; ++n) {
//		if (!checkChar(*(seq + n)))
//			return n;
//		++n;
//	}
//	return -1;
//}

std::string CheckFile(std::string filename, char const * const name) {
	if (!FileExists(filename.c_str())) {
		Log.Error("%s file not found (%s)", name, filename.c_str());
		Fatal();
	}
	return filename;
}

#include <stdio.h>
#include <zlib.h>

#include "Timing.h"

#include "kseq.h"

KSEQ_INIT(gzFile, gzread)

//static inline char enc4(char c) {
//	c = toupper(c);
//	switch (c) {
//	case 'A':
//		return 0x1;
//	case 'T':
//		return 0x4;
//	case 'G':
//		return 0x7;
//	case 'C':
//		return 0x3;
//	}
//	return 0xE;
//}
//
//static inline char dec4High(unsigned char c) {
//	c = (c >> 4) | 0x40;
//	if (c == 0x44)
//		c |= 0x10;
//	return c;
//}
//
//static inline char dec4Low(unsigned char c) {
//	c = (c & 0xF) | 0x40;
//	if (c == 0x44)
//		c |= 0x10;
//	return c;
//}

static inline char enc4(char c) {
	c = toupper(c);
	switch (c) {
		case 'A':
			return 0;
		case 'T':
			return 1;
		case 'G':
			return 2;
		case 'C':
			return 3;
	}
	return 4;
}

static inline char dec4(char c) {
	switch (c) {
		case 0:
			return 'A';
		case 1:
			return 'T';
		case 2:
			return 'G';
		case 3:
			return 'C';
		case 4:
			return 'N';
	}
	throw "Error in ref encoding!";
}

static inline char dec4High(unsigned char c) {
	return dec4(c >> 4);
}

static inline char dec4Low(unsigned char c) {
	return dec4(c & 0xF);
}

bool _SequenceProvider::convert(SequenceLocation & m_Location) {
	//Convert position back to Chromosome+Position
	SequenceLocation loc = m_Location;
	//Find the next larger chromosome start position in the concatenated reference for the mapping location
	uloc * upper = std::upper_bound(refStartPos, refStartPos + (refCount / ((DualStrand) ? 2 : 1)) + 1, loc.m_Location);

	//Check whether the mapping position is in one of the spacer regions between the chromosomes
	if ( (*upper - loc.m_Location) < ULOC_FROM_UINT32(1000)) { //TODO_GENOMESIZE: *upper has to be uloc size
		Log.Verbose("Read start position < chromosome start!");
		Log.Verbose("Loc: %u (%d) < %u < %u (%d)", (uloc)*(upper-1), ((upper - 2) - refStartPos) * ((DualStrand) ? 2 : 1), loc.m_Location, (uint)*(upper), ((upper - 1) - refStartPos) * ((DualStrand) ? 2 : 1));
		if(Config.Exists(ARGOS)) {
			//Set mapping position to start position of chromosome
			loc.m_Location = (uloc) *upper;
			upper += 1;
		} else {
			//Report read/position as unmapped (only happens for --end-to-end)
			return false;
		}
	}

	//Compute actual start position
	loc.m_Location -= *(upper - 1);
	std::ptrdiff_t refId = ((upper - 1) - refStartPos) * ((DualStrand) ? 2 : 1);

	Log.Verbose("Location: %u - Upper: %u - Location: %d, RefId: %d ", m_Location.m_Location, *(upper - 1), loc.m_Location, refId);
	loc.setRefId(refId);
	m_Location = loc;
	return true;
}

int _SequenceProvider::readEncRefFromFile(char const * fileName) {
	Log.Message("Reading encoded reference from %s", fileName);
	Timer wtmr;
	wtmr.ST();

	uloc encRefSize = MAKE_ULOC(0);
	uint refCount = 0;
	uint cookie = 0;

	FILE *fp;
	fp = fopen(fileName, "rb");

	fread(&cookie, sizeof(uint), 1, fp);
	fread(&refCount, sizeof(uint), 1, fp);
	fread(&binRefIndex, sizeof(uloc), 1, fp);
	fread(&encRefSize, sizeof(uloc), 1, fp);
	if (cookie != refEncCookie) {
		fclose(fp);
		Log.Error("Invalid encoded reference file found: %s.", fileName);
		Log.Error("Please delete it and run NGM again.");
		Fatal();
	}
	if(refCount > maxRefCount) {
		Log.Error("Currently NextGenMap can't handle more than %d reference sequences.", maxRefCount);
		Fatal();
	}
	binRefIdx = new RefIdx[refCount];
	fread(binRefIdx, sizeof(RefIdx), refCount, fp);

	binRef = new char[GET_ULOC(encRefSize)];
	fread(binRef, sizeof(char), GET_ULOC(encRefSize), fp);
	fclose(fp);
	Log.Message("Reading from disk took %.2fs", wtmr.ET());

	return refCount;
}

void _SequenceProvider::writeEncRefToFile(char const * fileName, uint const refCount, uloc const encRefSize) {
	if (!Config.GetInt("skip_save")) {
		Timer wtmr;
		wtmr.ST();
		Log.Message("Writing encoded reference to %s", fileName);
		FILE *fp;
		fp = fopen(fileName, "wb");
		fwrite(&refEncCookie, sizeof(uint), 1, fp);
		fwrite(&refCount, sizeof(uint), 1, fp);
		fwrite(&binRefIndex, sizeof(uloc), 1, fp);
		fwrite(&encRefSize, sizeof(uloc), 1, fp);

		fwrite(binRefIdx, sizeof(RefIdx), refCount, fp);
		fwrite(binRef, sizeof(char), GET_ULOC(encRefSize), fp);

		fclose(fp);
		Log.Message("Writing to disk took %.2fs", wtmr.ET());
	}
}

uloc getSize(char const * const file) { //TODO_GENOMESIZE: Does this work right?
	gzFile gzfp;
	kseq_t *seq;
	gzfp = gzopen(Config.GetString("ref"), "r");
	seq = kseq_init(gzfp);
	int l = 0;
	//1000 -> padding at beginning
	uloc size = ULOC_FROM_UINT32(1000);
	while ((l = kseq_read(seq)) >= 0) {
		//1000 -> 1000 x N padding
		int s = (seq->seq.l | 1) + 1;
		size += s + 1000;
	}
	kseq_destroy(seq);
	gzclose(gzfp);
	return size;
}

void _SequenceProvider::Init(bool dualstrand) {
	DualStrand = dualstrand;
	Log.Message("Init sequence provider.");
	if (!Config.Exists("ref")) {
		Log.Error("No reference file specified.");
		Fatal();
	}
//	if (NGM.Paired() && !Config.Exists("pqry")) {
//		Log.Error("No paired query file specified.");
//		Fatal();
//	}

	CheckFile(refBaseFileName = Config.GetString("ref"), "RefBase");

	refFileName = Config.GetString("ref") + std::string("-enc.ngm");

	if (FileExists(refFileName.c_str())) {
		//Read
		refCount = readEncRefFromFile(refFileName.c_str());
	} else {
		Log.Message("Encoding reference sequence.");
		std::map<int, RefIdx> binRefMap;

		uloc size = getSize(Config.GetString("ref"));
		refFileLen = size;

		Log.Message("Size of reference genome %llu (%llu)", GET_ULOC(size), ULLONG_MAX);
		/* If size > INT64_MAX, then we literally got a big problem
		if (size > ULLONG_MAX) {
			Log.Error("Reference genome too long! NGM can't handle genomes larger than %ld bytes.", ULLONG_MAX );
			Fatal();
		}*/

		uloc const binRefSize = ((size / 2) | 1) + 1;
		Log.Message("Allocating %llu (%llu) bytes for the reference.", GET_ULOC(binRefSize), GET_ULOC(FileSize(Config.GetString("ref")))); //TODO_GENOMESIZE: Check/adapt FileSize for uloc
		binRef = new char[GET_ULOC(binRefSize)];

		gzFile gzfp;
		kseq_t *seq;
		gzfp = gzopen(Config.GetString("ref"), "r");
		seq = kseq_init(gzfp);

		Timer tt;
		tt.ST();
		int l = 0;
		int j = 0;

		char const spacer = 'N';

		//Padding to avoid negative mapping positions
		for (int i = 0; i < 500; ++i) {
			char c = enc4(spacer) << 4;
			c |= enc4(spacer);
			binRef[GET_ULOC(binRefIndex++)] = c;
		}
		int skipped = 0;
		while ((l = kseq_read(seq)) >= 0) {
			if(j >= maxRefCount) {
				Log.Error("Currently NextGenMap can't handle more than %d reference sequences.", maxRefCount);
				Fatal();
			}
			if(seq->seq.l > minRefSeqLen) {
				binRefMap[j].SeqStart = binRefIndex * 2;
				binRefMap[j].SeqLen = seq->seq.l;
				binRefMap[j].SeqId = j;
				Log.Verbose("Ref %d: %s (%d), Index: %d", j, seq->name.s, seq->seq.l, binRefIndex);
				int nameLength = std::min((size_t) maxRefNameLength, seq->name.l);
				strncpy(binRefMap[j].name, seq->name.s, nameLength);
				binRefMap[j].NameLen = nameLength;
				j += 1;
				char * ref = seq->seq.s;
				for (size_t i = 0; i < seq->seq.l / 2 * 2; i += 2) {
					char c = enc4(ref[i]) << 4;
					c |= enc4(ref[i + 1]);
					binRef[GET_ULOC(binRefIndex++)] = c;
				}
				if (seq->seq.l & 1) {
					char c = enc4(ref[seq->seq.l - 1]) << 4;
					c |= enc4(spacer);
					binRef[GET_ULOC(binRefIndex++)] = c;
				}

				for (int i = 0; i < 500; ++i) {
					//N
					char c = enc4(spacer) << 4;
					c |= enc4(spacer);
					binRef[GET_ULOC(binRefIndex++)] = c;
				}
			} else {
				Log.Verbose("Reference sequence %s too short (%d). Skipping.", seq->name.s, seq->seq.l);
				skipped += 1;
			}
		}
		refCount = j;
		Log.Message("BinRef length: %ull (elapsed %f)", GET_ULOC(binRefIndex), tt.ET());
		Log.Message("%d reference sequences were skipped (length < %d).", skipped, minRefSeqLen);
		kseq_destroy(seq);
		gzclose(gzfp);

		binRefIndex = binRefIndex * 2;

		if (binRefMap.size() != (size_t) refCount) {
			Log.Error("Error while building ref index.");
			Fatal();
		}
		binRefIdx = new RefIdx[refCount];
		for (int i = 0; i < refCount; ++i) {
			if (binRefMap.find(i) != binRefMap.end()) {
				binRefIdx[i] = binRefMap[i];
//				binRefIdx[i].SeqStart = 0;
				//binRefIdx[i].SeqLen = 2 * binRefIndex - 1;
			} else {
				Log.Error("Error while building ref index.");
				Fatal();
			}
		}
		writeEncRefToFile(refFileName.c_str(), (uint) refCount, binRefSize);
	}

	if (DualStrand)
		refCount *= 2;

#ifdef VERBOSE
	for (int i = 0; i < refCount; ++i) {
		int len = 0;
		char const * test = GetRefName(i, len);
		Log.Message("%d: Ref: %.*s, Length: %d",i, len, test, (int)GetRefLen(i));
	}
#endif

	int refCount = SequenceProvider.GetRefCount();
	refStartPos = new uloc[refCount / ((DualStrand) ? 2 : 1) + 1];
	int i = 0;
	int j = 0;
	while (i < refCount) {
		refStartPos[j++] = SequenceProvider.GetRefStart(i);
		i += (DualStrand) ? 2 : 1;
	}
		//Add artificial start position as upper bound for all all reads that map to the last chromosome
	refStartPos[j] = refStartPos[j - 1] + SequenceProvider.GetRefLen(refCount - 1) + 1000;

}

bool _SequenceProvider::DecodeRefSequence(char * const buffer, int n, uloc offset, uloc bufferLength) {
	uloc len = bufferLength - 2;
	if (DualStrand) {
		n >>= 1;
	}
//	Log.Message("%u %u %u", offset, bufferLength, binRefIdx[n].SeqLen);
	//if (offset >= binRefIdx[n].SeqLen || offset < 0) {
	if (offset >= GetConcatRefLen()) {
		Log.Verbose("Invalid reference location. Offset: %d", offset);
		return false;
	}
//	int nCount = 0;
//	if (offset < 0) {
//		nCount = abs(offset);
//		len -= nCount;
//		offset = 0;
//	}
	uloc end = ULOC_FROM_UINT32(0);
	if ((offset + len) > GetConcatRefLen()) {
		end = (offset + len) - GetConcatRefLen();
		len -= end;
	}
//	uint end = std::min((uint)0, binRefIdx[n].SeqLen - (offset + len));
//	if (end < 0) {
//		end = abs(end);
//		len -= end;
//	}
//	int start = binRefIdx[n].SeqStart + ceil(offset / 2.0);
	uloc start = (offset + 1)/2; //TODO: Check if equiv to ceil(offset / 2.0);

	uint codedIndex = 0;
//	for (int i = 0; i < nCount; ++i) {
//		buffer[codedIndex++] = 'x';
//	}
	if (GET_ULOC(offset & 1)) {
		buffer[codedIndex++] = dec4Low(binRef[GET_ULOC(start - 1)]);
	}
	for (uloc i = ULOC_FROM_UINT32(0); i < (len+1)/2; ++i) { //TODO_GENOMESIZE: i < ceil(len / 2.0)
		buffer[codedIndex++] = dec4High(binRef[GET_ULOC(start + i)]);
		buffer[codedIndex++] = dec4Low(binRef[GET_ULOC(start + i)]);
	}
	if (GET_ULOC(len & 1)) {
		buffer[codedIndex - 1] = 'x';
	}
	for (uloc i = ULOC_FROM_UINT32(0); i < end; ++i) {
		buffer[codedIndex++] = 'x';
	}

	if (codedIndex > bufferLength) {
		Log.Error("nCount: %d, offset: %d, len: %d (%d), seqlen: %d, end: %d, start: %d, index: %d", 0, GET_ULOC(offset), GET_ULOC(bufferLength), GET_ULOC((len+1)/2), binRefIdx[n].SeqLen, GET_ULOC(end), GET_ULOC(start), codedIndex);
		Log.Error("%.*s", GET_ULOC(bufferLength), buffer);
		Fatal();
	}

	for (uint i = codedIndex; i < bufferLength; ++i) {
		buffer[i] = '\0';
	}
	return true;
}

char const * _SequenceProvider::GetRefName(int n, int & len) const {
	if (CheckRefNr(n)) {
		if (DualStrand)
			n >>= 1;
		len = binRefIdx[n].NameLen;

		return binRefIdx[n].name;
	}
	return 0;
}

uloc _SequenceProvider::GetConcatRefLen() const {
	return binRefIndex - 1;
}

uloc _SequenceProvider::GetRefLen(int n) const {
	if (CheckRefNr(n)) {
		if (DualStrand)
			n >>= 1;
		return ULOC_FROM_UINT32(binRefIdx[n].SeqLen);
	} else {
		return ULOC_FROM_UINT32(0);
	}
}

uloc _SequenceProvider::GetRefStart(int n) const {
	if (CheckRefNr(n)) {
		if (DualStrand)
			n >>= 1;
		return binRefIdx[n].SeqStart;
	} else {
		return ULOC_FROM_UINT32(0);
	}
}

int _SequenceProvider::GetRefCount() const {
	return refCount;
}

bool _SequenceProvider::CheckRefNr(int n) const {
	if (n >= refCount || n < 0) {
		Log.Error("Tried to access invalid reference sequence (%i %x).", n, n);
		throw "Problem";
		//Fatal();
		return false;
	}
	return true;
}

_SequenceProvider::_SequenceProvider() :
		 binRef(0), refStartPos(0), refCount(0), m_EnableBS(false) {

			binRefIndex = ULOC_FROM_UINT32(0);
}

_SequenceProvider::~_SequenceProvider() {
	delete[] binRef;
	binRef = 0;
	delete[] refStartPos;
	refStartPos = 0;
}

void _SequenceProvider::Cleanup() {
	delete pInstance;
}

void _SequenceProvider::PagingUpdate() {
	//TODO: remove
}
