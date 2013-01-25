'''
This script writes the blob of data below to the SD card connected to a bot, 
it then compares the data on the SD to identical data saved in the PROGMEM of the firmware.

to use load the firmware included in this directory to a Rep2,
then run this script with a filename and port specified
'''

import os
import sys
import time
import optparse

sys.path.append(os.path.abspath('./s3g'))
import makerbot_driver

DATA = [141, 73, 87, 109, 163, 218, 26, 139, 196, 103, 8, 154, 102, 15, 13, 67, 194, 179, 90, 161, 157, 243, 37, 145, 68, 8, 146, 226, 165, 195, 219, 231, 24, 3, 34, 248, 254, 160, 202, 53, 99, 254, 6, 4, 41, 229, 45, 54, 92, 250, 87, 64, 38, 50, 140, 134, 182, 3, 246, 114, 251, 197, 80, 165, 217, 160, 35, 172, 245, 216, 17, 157, 18, 11, 7, 29, 107, 135, 255, 185, 82, 235, 23, 79, 71, 149, 147, 30, 65, 133, 221, 220, 102, 139, 196, 83, 194, 129, 55, 226, 5, 159, 107, 236, 198, 249, 54, 245, 42, 114, 27, 147, 244, 61, 242, 31, 75, 158, 121, 170, 193, 225, 241, 26, 223, 41, 203, 178, 77, 237, 20, 152, 247, 52, 75, 176, 28, 55, 234, 185, 72, 63, 247, 90, 24, 232, 36, 232, 177, 149, 243, 171, 129, 249, 57, 166, 100, 1, 228, 93, 44, 210, 204, 113, 183, 85, 46, 71, 154, 246, 37, 238, 148, 171, 81, 124, 97, 182, 88, 112, 111, 105, 138, 78, 101, 188, 61, 27, 109, 200, 57, 110, 10, 223, 116, 47, 22, 251, 79, 131, 103, 137, 21, 19, 0, 127, 201, 138, 30, 7, 156, 106, 40, 186, 227, 82, 16, 50, 253, 17, 32, 135, 96, 126, 137, 85, 228, 44, 198, 142, 35, 212, 100, 187, 120, 222, 252, 111, 209, 72, 192, 105, 175, 153, 9, 230, 58, 195, 112, 214, 155, 123, 43, 91, 77, 248, 255, 124, 163, 191, 15, 187, 69, 68, 134, 52, 176, 14, 74, 250, 67, 86, 40, 210, 130, 148, 122, 60, 207, 192, 12, 5, 131, 172, 174, 211, 84, 59, 235, 240, 213, 155, 146, 189, 133, 152, 177, 128, 89, 153, 150, 190, 191, 120, 234, 101, 167, 86, 33, 125, 104, 169, 143, 205, 63, 143, 208, 140, 168, 118, 207, 173, 98, 25, 174, 253, 13, 181, 173, 4, 91, 125, 225, 46, 186, 28, 95, 144, 142, 59, 181, 0, 126, 233, 168, 244, 96, 158, 92, 53, 151, 212, 89, 123, 200, 220, 211, 242, 64, 10, 9, 221, 204, 239, 51, 190, 94, 215, 38, 206, 62, 51, 118, 34, 48, 169, 162, 56, 150, 84, 117, 203, 20, 206, 161, 104, 58, 132, 236, 60, 237, 213, 108, 218, 216, 219, 178, 49, 162, 98, 11, 183, 108, 215, 110, 224, 240, 167, 113, 1, 205, 83, 208, 62, 209, 39, 99, 95, 175, 141, 116, 197, 227, 145, 93, 238, 127, 12, 136, 19, 45, 47, 159, 97, 224, 136, 180, 151, 39, 239, 229, 189, 115, 214, 128, 69, 121, 184, 230, 66, 188, 241, 2, 14, 43, 119, 202, 23, 49, 164, 21, 70, 132, 179, 48, 16, 29, 156, 80, 70, 33, 170, 122, 31, 74, 233, 73, 25, 18, 36, 231, 164, 115, 94, 130, 76, 184, 193, 42, 32, 180, 106, 78, 217, 119, 222, 6, 81, 117, 199, 88, 201, 252, 76, 166, 65, 144, 66, 2, 56, 22, 199]

parser = optparse.OptionParser()

parser.add_option("-f", "--filename", dest="filename")
parser.add_option("-p", "--port", dest="port")
(options, args) = parser.parse_args()

driver_obj = makerbot_driver.s3g.from_filename(options.port)

number_true = 0
number_false = 0
number_other = 0
r_count = 0
w_count = 0

while(w_count < 10):

	driver_obj.capture_to_file(options.filename)
	driver_obj.send_raw_data(DATA)
	driver_obj.end_capture_to_file()

	w_count += 1

	while(r_count < 1000):

		(return_val, bot_return) = driver_obj.cmp_files(options.filename)


		if(return_val == True):
		    number_true += 1
		    print('True')
		elif(return_val == False):
		    number_false += 1
		    print('False')
		else:
		    number_other += 1
		    print('Other/Corrupt/Error')
		print(str(bot_return))
		r_count += 1

print("{0} / 10000 return values were true".format(number_true))
print("{0} / 10000 return values were false".format(number_false))
print("{0} / 10000 return values were corrupt/malformed".format(number_other))
