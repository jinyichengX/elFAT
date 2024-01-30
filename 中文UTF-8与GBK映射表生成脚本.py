# -*- coding: utf-8 -*-

with open("中文编码表.txt", "w+") as wf:
    for i in range(0x4e00, 0x9fa5 + 1):
        ustr = ("\u" + hex(i)[2:6]).encode("utf-8").decode("unicode_escape")
        utf8bytes = ustr.encode("utf-8")
        utf8hex = [hex(utf8bytes[0]), hex(utf8bytes[1]), hex(utf8bytes[2])]
        gbkbytes = ustr.encode("gbk")
        gbkhex = [hex(gbkbytes[0]), hex(gbkbytes[1])]
        text = ustr + " " + str(utf8hex).replace("'", "") + " " + str(gbkhex).replace("'", "")
        print(text)
        wf.write(text + "
")
