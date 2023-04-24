// Generated automatically with "cito". Do not edit.
#include <stdlib.h>
#include <string.h>
#include "aatr.h"

static void CiString_Assign(char **str, char *value)
{
	free(*str);
	*str = value;
}

static char *CiString_Substring(const char *str, int len)
{
	char *p = malloc(len + 1);
	memcpy(p, str, len);
	p[len] = '\0';
	return p;
}

static void CiString_AppendSubstring(char **str, const char *suffix, size_t suffixLen)
{
	if (suffixLen == 0)
		return;
	size_t prefixLen = *str == NULL ? 0 : strlen(*str);
	*str = realloc(*str, prefixLen + suffixLen + 1);
	memcpy(*str + prefixLen, suffix, suffixLen);
	(*str)[prefixLen + suffixLen] = '\0';
}

static void CiString_Append(char **str, const char *suffix)
{
	CiString_AppendSubstring(str, suffix, strlen(suffix));
}

typedef struct {
	bool (*read)(const AATR *self, int offset, uint8_t *buffer, int length);
} AATRVtbl;
/**
 * ATR disk image reader.
 */
struct AATR {
	const AATRVtbl *vtbl;
	int bytesPerSector;
	int sector4Offset;
};

static bool AATR_ReadSector(const AATR *self, int sectorNo, uint8_t *buffer, int length);

/**
 * Forward-only iterator over a directory on an ATR disk image.
 */
struct AATRDirectory {
	const AATR *disk;
	int firstSector;
	int fileNo;
	uint8_t sector[128];
	char *filename;
};
static void AATRDirectory_Construct(AATRDirectory *self);
static void AATRDirectory_Destruct(AATRDirectory *self);

static int AATRDirectory_GetFilenamePart(const AATRDirectory *self, int sectorOffset, int maxLength);

static int AATRDirectory_GetEntryType(const AATRDirectory *self);

static int AATRDirectory_GetEntryFirstSector(const AATRDirectory *self);

/**
 * Iterator over all files on an ATR disk image.
 */
struct AATRRecursiveLister {
	AATRDirectory directories[20];
	int depth;
	char *directoryPath;
	char *filePath;
};
static void AATRRecursiveLister_Construct(AATRRecursiveLister *self);
static void AATRRecursiveLister_Destruct(AATRRecursiveLister *self);

/**
 * Contents of a file on an ATR disk image.
 */
struct AATRFileStream {
	const AATR *disk;
	int firstSector;
	int fileType;
	int position;
	int nextSector;
	uint8_t sector[256];
	int sectorOffset;
};
static void AATRFileStream_Construct(AATRFileStream *self);

static void AATRFileStream_Rewind(AATRFileStream *self);

bool AATR_Open(AATR *self)
{
	uint8_t header[6];
	if (!self->vtbl->read(self, 0, header, 6) || header[0] != 150 || header[1] != 2)
		return false;
	self->bytesPerSector = header[4] | header[5] << 8;
	self->sector4Offset = 400;
	switch (self->bytesPerSector) {
	case 128:
		break;
	case 256:
		if ((header[2] & 15) == 0)
			self->sector4Offset = 784;
		break;
	default:
		return false;
	}
	return true;
}

int AATR_GetBytesPerSector(const AATR *self)
{
	return self->bytesPerSector;
}

static bool AATR_ReadSector(const AATR *self, int sectorNo, uint8_t *buffer, int length)
{
	int offset;
	if (sectorNo < 4) {
		if (sectorNo < 1 || length > 128)
			return false;
		offset = (sectorNo << 7) - 112;
	}
	else
		offset = self->sector4Offset + (sectorNo - 4) * self->bytesPerSector;
	return self->vtbl->read(self, offset, buffer, length);
}

static void AATRDirectory_Construct(AATRDirectory *self)
{
	self->filename = NULL;
}

static void AATRDirectory_Destruct(AATRDirectory *self)
{
	free(self->filename);
}

AATRDirectory *AATRDirectory_New(void)
{
	AATRDirectory *self = (AATRDirectory *) malloc(sizeof(AATRDirectory));
	if (self != NULL)
		AATRDirectory_Construct(self);
	return self;
}

void AATRDirectory_Delete(AATRDirectory *self)
{
	if (self == NULL)
		return;
	AATRDirectory_Destruct(self);
	free(self);
}

void AATRDirectory_OpenRoot(AATRDirectory *self, const AATR *disk)
{
	self->disk = disk;
	self->firstSector = 361;
	self->fileNo = -1;
}

static int AATRDirectory_GetFilenamePart(const AATRDirectory *self, int sectorOffset, int maxLength)
{
	for (int length = 0; length < maxLength; length++) {
		int c = self->sector[sectorOffset + length];
		if (c == ' ') {
			int result = length;
			while (++length < maxLength) {
				if (self->sector[sectorOffset + length] != ' ')
					return -1;
			}
			return result;
		}
		if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
			continue;
		return -1;
	}
	return maxLength;
}

const char *AATRDirectory_NextEntry(AATRDirectory *self)
{
	for (;;) {
		if (self->fileNo >= 63)
			return NULL;
		self->fileNo++;
		int sectorOffset = (self->fileNo & 7) << 4;
		if (sectorOffset == 0 && !AATR_ReadSector(self->disk, self->firstSector + (self->fileNo >> 3), self->sector, 128))
			return NULL;
		switch (self->sector[sectorOffset] & 215) {
		case 0:
			return NULL;
		case 64:
			if (AATR_GetBytesPerSector(self->disk) != 128)
				continue;
			break;
		case 66:
		case 3:
		case 70:
		case 16:
			break;
		default:
			continue;
		}
		int filenameLength = AATRDirectory_GetFilenamePart(self, sectorOffset + 5, 8);
		if (filenameLength < 0)
			continue;
		int extLength = AATRDirectory_GetFilenamePart(self, sectorOffset + 13, 3);
		if (extLength < 0 || (filenameLength == 0 && extLength == 0))
			continue;
		CiString_Assign(&self->filename, CiString_Substring((const char *) self->sector + sectorOffset + 5, filenameLength));
		if (extLength > 0) {
			self->sector[sectorOffset + 12] = '.';
			char *ext = CiString_Substring((const char *) self->sector + sectorOffset + 12, 1 + extLength);
			CiString_Append(&self->filename, ext);
			free(ext);
		}
		return self->filename;
	}
}

static int AATRDirectory_GetEntryType(const AATRDirectory *self)
{
	int sectorOffset = (self->fileNo & 7) << 4;
	return self->sector[sectorOffset] & 215;
}

bool AATRDirectory_IsEntryDirectory(const AATRDirectory *self)
{
	return AATRDirectory_GetEntryType(self) == 16;
}

static int AATRDirectory_GetEntryFirstSector(const AATRDirectory *self)
{
	int sectorOffset = (self->fileNo & 7) << 4;
	return self->sector[sectorOffset + 3] | self->sector[sectorOffset + 4] << 8;
}

void AATRDirectory_Open(AATRDirectory *self, const AATRDirectory *directory)
{
	self->disk = directory->disk;
	self->firstSector = AATRDirectory_GetEntryFirstSector(directory);
	self->fileNo = -1;
}

bool AATRDirectory_FindEntry(AATRDirectory *self, const char *filename)
{
	for (;;) {
		const char *currentFilename = AATRDirectory_NextEntry(self);
		if (currentFilename == NULL)
			return false;
		if (strcmp(currentFilename, filename) == 0)
			return true;
	}
}

bool AATRDirectory_FindEntryRecursively(AATRDirectory *self, const char *path)
{
	int i = 0;
	int pathLength = (int) strlen(path);
	for (int j = 0; j < pathLength; j++) {
		if (path[j] == '/') {
			if (j - i > 12)
				return false;
			char *dirname = CiString_Substring(path + i, j - i);
			if (!AATRDirectory_FindEntry(self, dirname) || !AATRDirectory_IsEntryDirectory(self)) {
				free(dirname);
				return false;
			}
			AATRDirectory_Open(self, self);
			i = j + 1;
			free(dirname);
		}
	}
	if (pathLength - i > 12)
		return false;
	char *filename = CiString_Substring(path + i, pathLength - i);
	bool returnValue = AATRDirectory_FindEntry(self, filename);
	free(filename);
	return returnValue;
}

static void AATRRecursiveLister_Construct(AATRRecursiveLister *self)
{
	for (int _i0 = 0; _i0 < 20; _i0++) {
		AATRDirectory_Construct(&self->directories[_i0]);
	}
	self->directoryPath = NULL;
	self->filePath = NULL;
}

static void AATRRecursiveLister_Destruct(AATRRecursiveLister *self)
{
	free(self->filePath);
	free(self->directoryPath);
	for (int _i0 = 19; _i0 >= 0; _i0--)
		AATRDirectory_Destruct(&self->directories[_i0]);
}

AATRRecursiveLister *AATRRecursiveLister_New(void)
{
	AATRRecursiveLister *self = (AATRRecursiveLister *) malloc(sizeof(AATRRecursiveLister));
	if (self != NULL)
		AATRRecursiveLister_Construct(self);
	return self;
}

void AATRRecursiveLister_Delete(AATRRecursiveLister *self)
{
	if (self == NULL)
		return;
	AATRRecursiveLister_Destruct(self);
	free(self);
}

void AATRRecursiveLister_Open(AATRRecursiveLister *self, const AATR *disk)
{
	AATRDirectory_OpenRoot(&self->directories[0], disk);
	self->depth = 0;
	CiString_Assign(&self->directoryPath, _strdup(""));
}

const char *AATRRecursiveLister_NextFile(AATRRecursiveLister *self)
{
	for (;;) {
		AATRDirectory *directory = &self->directories[self->depth];
		const char *filename = AATRDirectory_NextEntry(directory);
		if (filename == NULL) {
			if (self->depth == 0)
				return NULL;
			int i = (int) strlen(self->directoryPath) - 1;
			while (--i >= 0 && self->directoryPath[i] != '/') {
			}
			self->directoryPath[i + 1] = '\0';
			self->depth--;
		}
		else if (AATRDirectory_IsEntryDirectory(directory)) {
			if (self->depth >= 19)
				continue;
			CiString_Append(&self->directoryPath, filename);
			CiString_Append(&self->directoryPath, "/");
			AATRDirectory_Open(&self->directories[++self->depth], directory);
		}
		else {
			CiString_Assign(&self->filePath, _strdup(self->directoryPath));
			CiString_Append(&self->filePath, filename);
			return self->filePath;
		}
	}
}

const AATRDirectory *AATRRecursiveLister_GetDirectory(const AATRRecursiveLister *self)
{
	return &self->directories[self->depth];
}

static void AATRFileStream_Construct(AATRFileStream *self)
{
}

AATRFileStream *AATRFileStream_New(void)
{
	AATRFileStream *self = (AATRFileStream *) malloc(sizeof(AATRFileStream));
	if (self != NULL)
		AATRFileStream_Construct(self);
	return self;
}

void AATRFileStream_Delete(AATRFileStream *self)
{
	free(self);
}

static void AATRFileStream_Rewind(AATRFileStream *self)
{
	self->position = 0;
	self->nextSector = self->firstSector;
	self->sectorOffset = 255;
}

void AATRFileStream_Open(AATRFileStream *self, const AATRDirectory *directory)
{
	self->disk = directory->disk;
	self->firstSector = AATRDirectory_GetEntryFirstSector(directory);
	self->fileType = AATRDirectory_GetEntryType(directory) & 6;
	AATRFileStream_Rewind(self);
}

int AATRFileStream_Read(AATRFileStream *self, uint8_t *buffer, int offset, int length)
{
	int totalRead = 0;
	int bytesPerSector = AATR_GetBytesPerSector(self->disk);
	while (length > 0) {
		int got = self->sector[bytesPerSector - 1];
		if (self->fileType == 0) {
			if (got < 128)
				got = 125;
			else
				got -= 128;
		}
		else if (got > bytesPerSector)
			return -1;
		got -= self->sectorOffset;
		if (got <= 0) {
			if (!AATR_ReadSector(self->disk, self->nextSector, self->sector, bytesPerSector))
				return totalRead;
			self->sectorOffset = 0;
			int sectorHi = self->sector[bytesPerSector - 3];
			if (self->fileType != 6)
				sectorHi &= 3;
			self->nextSector = sectorHi << 8 | self->sector[bytesPerSector - 2];
		}
		else {
			if (got > length)
				got = length;
			if (buffer != NULL)
				memcpy(buffer + offset + totalRead, self->sector + self->sectorOffset, got);
			self->position += got;
			self->sectorOffset += got;
			totalRead += got;
			length -= got;
		}
	}
	return totalRead;
}

const AATR *AATRFileStream_GetDisk(const AATRFileStream *self)
{
	return self->disk;
}

int AATRFileStream_GetPosition(const AATRFileStream *self)
{
	return self->position;
}

bool AATRFileStream_SetPosition(AATRFileStream *self, int position)
{
	int toSkip = position - self->position;
	if (toSkip >= 0)
		return AATRFileStream_Read(self, NULL, 0, toSkip) == toSkip;
	AATRFileStream_Rewind(self);
	return AATRFileStream_Read(self, NULL, 0, position) == position;
}

int AATRFileStream_GetLength(AATRFileStream *self)
{
	int position = AATRFileStream_GetPosition(self);
	int length = position + AATRFileStream_Read(self, NULL, 0, 2147483647 - position);
	AATRFileStream_SetPosition(self, position);
	return length;
}
