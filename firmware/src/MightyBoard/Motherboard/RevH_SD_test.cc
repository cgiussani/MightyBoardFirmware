#include "RevH_SD_test.hh"

//Compares a file in the SD to SingleHeadStart in PROGMEM, This is for SD read reliability testing
uint8_t cmp_SdFile_to_MemFile(char* filename) {

	bool hasMoreData;
	uint8_t fileByte; //byte of the SD file to compare
	uint16_t byteIndex;
	uint32_t fileSize;

	sdcard::SdErrorCode result = sdcard::startFileRead(filename);

	if(result != sdcard::SD_SUCCESS) {
		return -1;
	}

	fileSize = sdcard::getFileSize();
	if(fileSize != sizeof(SingleHeadStart)){
		return 0;
	}

	while(hasMoreData) {

		fileByte = sdcard::playbackNext();

		if(fileByte != SingleHeadStart[byteIndex]) {
			return 0;
		}
		byteIndex++;

		hasMoreData = sdcard::playbackHasNext();
	}

	return 255;
}
