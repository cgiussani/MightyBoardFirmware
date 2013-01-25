import os
import sys
import time
import optparse

sys.path.append(os.path.abspath('./s3g'))
import makerbot_driver

parser = optparse.OptionParser()

parser.add_option("-f", "--filename", dest="filename_cmp")
parser.add_option("-p", "--port", dest="port")
(options, args) = parser.parse_args()

driver_obj = makerbot_driver.s3g.from_filename(options.port)

number_true = 0
number_false = 0
number_other = 0
count = 0
while(count < 1000):

    (return_val, bot_return) = driver_obj.cmp_files(options.filename_cmp)


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
    count += 1

print("{0} / 1000 return values were true".format(number_true))
print("{0} / 1000 return values were false".format(number_false))
print("{0} / 1000 return values were corrupt/malformed".format(number_other))
