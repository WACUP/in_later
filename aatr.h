// Generated automatically with "cito". Do not edit.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct AATR AATR;
typedef struct AATRDirectory AATRDirectory;
typedef struct AATRRecursiveLister AATRRecursiveLister;
typedef struct AATRFileStream AATRFileStream;

/**
 * Opens an ATR disk image.
 * Returns <code>true</code> on success.
 * @param self This <code>AATR</code>.
 */
bool AATR_Open(AATR *self);

/**
 * Returns sector size in bytes.
 * @param self This <code>AATR</code>.
 */
int AATR_GetBytesPerSector(const AATR *self);

AATRDirectory *AATRDirectory_New(void);
void AATRDirectory_Delete(AATRDirectory *self);

/**
 * Opens the root directory of the specified disk image.
 * @param self This <code>AATRDirectory</code>.
 * @param disk The disk image.
 */
void AATRDirectory_OpenRoot(AATRDirectory *self, const AATR *disk);

/**
 * Advances to the next entry (file or directory).
 * Returns the filename or <code>null</code> if no more entries found.
 * @param self This <code>AATRDirectory</code>.
 */
const char *AATRDirectory_NextEntry(AATRDirectory *self);

/**
 * Returns <code>true</code> if the current entry is a directory.
 * @param self This <code>AATRDirectory</code>.
 */
bool AATRDirectory_IsEntryDirectory(const AATRDirectory *self);

/**
 * Opens the specified subdirectory.
 * @param self This <code>AATRDirectory</code>.
 * @param directory The iterator pointing at the subdirectory to open.
 */
void AATRDirectory_Open(AATRDirectory *self, const AATRDirectory *directory);

/**
 * Advances to the specified entry.
 * Returns whether the entry is found.
 * @param self This <code>AATRDirectory</code>.
 * @param filename The name of the file or directory to find.
 */
bool AATRDirectory_FindEntry(AATRDirectory *self, const char *filename);

/**
 * Advances to the specified entry, which may be in a subdirectory.
 * Returns whether the entry is found.
 * @param self This <code>AATRDirectory</code>.
 * @param path The name of the file or directory to find, with subdirectories separated with slashes.
 */
bool AATRDirectory_FindEntryRecursively(AATRDirectory *self, const char *path);

AATRRecursiveLister *AATRRecursiveLister_New(void);
void AATRRecursiveLister_Delete(AATRRecursiveLister *self);

/**
 * Opens a disk image.
 * @param self This <code>AATRRecursiveLister</code>.
 * @param disk The disk image to list files on.
 */
void AATRRecursiveLister_Open(AATRRecursiveLister *self, const AATR *disk);

/**
 * Advances to the next file.
 * Returns the full path of the file or <code>null</code> if no more files found.
 * @param self This <code>AATRRecursiveLister</code>.
 */
const char *AATRRecursiveLister_NextFile(AATRRecursiveLister *self);

/**
 * Returns the directory iterator pointing at the file found by <code>NextFile</code>.
 * @param self This <code>AATRRecursiveLister</code>.
 */
const AATRDirectory *AATRRecursiveLister_GetDirectory(const AATRRecursiveLister *self);

AATRFileStream *AATRFileStream_New(void);
void AATRFileStream_Delete(AATRFileStream *self);

/**
 * Opens a file.
 * @param self This <code>AATRFileStream</code>.
 * @param directory Directory iterator pointing at the file to open.
 */
void AATRFileStream_Open(AATRFileStream *self, const AATRDirectory *directory);

/**
 * Reads from the open file.
 * Returns the number of bytes read.
 * @param self This <code>AATRFileStream</code>.
 * @param buffer Destination buffer.
 * @param offset Start offset in buffer at which the data is written.
 * @param length Maximum number of bytes to read.
 */
int AATRFileStream_Read(AATRFileStream *self, uint8_t *buffer, int offset, int length);

/**
 * Returns the disk image containing this file.
 * @param self This <code>AATRFileStream</code>.
 */
const AATR *AATRFileStream_GetDisk(const AATRFileStream *self);

/**
 * Returns the current position in the open file.
 * @param self This <code>AATRFileStream</code>.
 */
int AATRFileStream_GetPosition(const AATRFileStream *self);

/**
 * Sets the current position in the open file.
 * Returns <code>true</code> on success.
 * @param self This <code>AATRFileStream</code>.
 * @param position Requested position.
 */
bool AATRFileStream_SetPosition(AATRFileStream *self, int position);

/**
 * Returns the length of the open file.
 * @param self This <code>AATRFileStream</code>.
 */
int AATRFileStream_GetLength(AATRFileStream *self);

#ifdef __cplusplus
}
#endif
