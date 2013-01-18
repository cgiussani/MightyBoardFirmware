#include "RevH_SD_test.hh"

//Compares a file in the SD to SingleHeadStart in PROGMEM, This is for SD read reliability testing
uint8_t cmp_SdFile_to_MemFile(char* filename) {

	bool hasMoreData;
	uint8_t fileByte; //byte of the SD file to compare
	uint16_t i;
	uint32_t fileSize;

	sdcard::SdErrorCode result = sdcard::startFileRead(filename);

	if(result != sdcard::SD_SUCCESS) {
		return (uint8_t)result;
	}

	if(sdcard::getFileSize() != sizeof(TEST_CMP)){
		return 0x00;
	}

    i = 0;

	do{

		fileByte = sdcard::playbackNext();
        
		if(pgm_read_byte(&TEST_CMP[i]) != fileByte)
        {
            return 0x00;
		}
		i++;

		hasMoreData = sdcard::playbackHasNext();
	}while(hasMoreData);

	return 0xFF;

}
