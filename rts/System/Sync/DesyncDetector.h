/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef DESYNCDETECTOR_H
#define DESYNCDETECTOR_H

#include <stdio.h>
// This is an ultra-lightweight desync detector, that detects the first call to
// CSyncChecker::Sync that generates a different checksum compared to a previous run.
// It is particularly useful for finding desyncs caused by multithreaded simulation
namespace DesyncDetector {
#ifdef USE_DESYNC_DETECTOR
	extern bool desyncDetectorActive;
	extern void DesyncTriggerFunc();
	extern FILE* desyncWriteFile;
	extern FILE* desyncReadFile;
	extern char* desyncErrorFileName;
	inline void StartPlaying() { desyncDetectorActive = true; }
	inline void Sync(const void* p, unsigned size, unsigned int checkSum) {
		if(desyncDetectorActive) {
			if (desyncReadFile != NULL) {
				int sz, chk;
				if(fread(&sz, sizeof(sz), 1, desyncReadFile) && fread(&chk, sizeof(chk), 1, desyncReadFile)) {
					if (checkSum != chk) {
						FILE *desyncErrorFile = fopen(desyncErrorFileName,"wb");
						if (desyncErrorFile != NULL) {
							fprintf(desyncErrorFile, "THIS GAME size: %d   checksum: %d\r\nData: ", size, checkSum);
							for(int i = 0; i < size; ++i)
								fprintf(desyncErrorFile, "0x%x ", (unsigned int)((unsigned char *)p)[i]);
							fprintf(desyncErrorFile, "\r\n\r\nSYNC FILE size: %d   checksum: %d\r\nData: ", sz, chk);
							for(int i = 0; i < sz; ++i)
								fprintf(desyncErrorFile, "0x%x ", (unsigned int)fgetc(desyncReadFile));
							fclose(desyncErrorFile);
						}
						DesyncTriggerFunc();
					}
					else {
						for(int i = 0; i < sz; ++i) {
							fgetc(desyncReadFile);
						}
					}
				} else {
					DesyncTriggerFunc();
				}
			} else if(desyncWriteFile != NULL) {
				fwrite(&size, sizeof(size), 1, desyncWriteFile);
				fwrite(&checkSum, sizeof(checkSum), 1, desyncWriteFile);
				for(int i = 0; i<size; ++i) {
					fputc(((char *)p)[i], desyncWriteFile);
				}
			}
		}
	}
#else
	inline void StartPlaying() {}
	inline void Sync(const void* p, unsigned size, unsigned int checkSum) {}
#endif
}

#endif // DESYNCDETECTOR_H
