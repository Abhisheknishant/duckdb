#!/usr/bin/python

import os, time

last_format_file = '.last_format'
format_command = 'clang-format -i -style=file "${FILE}"'
extensions = ['.cpp', '.c', '.hpp', '.h']

# get the last time this command was run, if ever

last_format_time = 0

if os.path.exists(last_format_file):
	with open(last_format_file, 'r') as f:
		try:
			last_format_time = float(f.read())
		except:
			pass


if last_format_time > 0:
	print('Last format: %s' % (time.ctime(last_format_time)))


time.ctime(os.path.getmtime('tools/sqlite3_api_wrapper/include/sqlite3.h'))

def format_directory(directory):
	directory_printed = False
	files = os.listdir(directory)
	for f in files:
		full_path = os.path.join(directory, f)
		if os.path.isdir(full_path):
			format_directory(full_path)
		else:
			# don't format TPC-H constants
			if 'tpch_constants.hpp' in full_path:
				continue
			for ext in extensions:
				if f.endswith(ext):
					if os.path.getmtime(full_path) > last_format_time:
						if not directory_printed:
							print(directory)
							directory_printed = True
						cmd = format_command.replace("${FILE}", full_path)
						print(cmd)
						os.system(cmd)
					break

format_directory('src')
format_directory('test')
format_directory('third_party/dbgen')
format_directory('tools')

# write the last modified time
with open(last_format_file, 'w+') as f:
	f.write(str(time.time()))
