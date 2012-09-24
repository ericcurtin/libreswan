#!/usr/bin/env python
import pexpect
import sys
import getopt, sys
import time
import os 
import setproctitle

def main():
	options, remainder = getopt.gnu_getopt(sys.argv[1:], 'h', ['help', 'host=', 'test=', ])
	
	for opt, arg in options:
		if opt in ('-h', '--help'):
			print 'help. one day it will be there'
			sys.exit(1)
		elif  opt in ('--host'):
			vmhost = arg
		elif opt in ('--test'):
			testname = arg

	print 'HOST : ', vmhost 
	print 'TEST : ', testname

	cmd = "%s-%s" % (sys.argv,vmhost)
	setproctitle.setproctitle(cmd)

	output_file = "./OUTPUT/27%s.console.txt" % (vmhost)
	f = open(output_file, 'w') 
	
	cmd = "sudo virsh reset %s" % (vmhost)
	r =  pexpect.spawn (cmd)
	time.sleep( 2 )

	cmd = "sudo virsh console %s" % (vmhost)
	child = pexpect.spawn (cmd)
	child.logfile = f
	time.sleep( 2 )

	a = child.expect (['Escape.*', 'Active console session exists for this domain'])
	if a==0:
		child.sendline ('')

	i = child.expect (['login: ', 'Active console session exists'], timeout=120) 
	if i==0:
		child.sendline ('root')
		child.expect ('Password:')
		child.sendline ('swan')
		child.expect ('root.*')
		print  'logged in as root'
	elif i==1:
		print 'console is busy'
		sys.exit(1) 

	prompt = "root@%s %s" % (vmhost, testname) 

	cmd = "cd /testing/pluto/%s " % (testname)
	print cmd
	child.sendline(cmd)
	child.expect (prompt, searchwindowsize=100) 

	cmd = ".  ./testparams.sh"
	print cmd
	child.sendline(cmd)  
	child.expect (prompt, searchwindowsize=100) 

	cmd = "./%sinit.sh" %  (vmhost) 
	print cmd
	child.sendline(cmd)  
	child.expect (prompt,timeout=180, searchwindowsize=100) 

	cmd = "./%srun.sh" %  (vmhost) 
	if os.path.exists(cmd):
		print cmd
		child.sendline(cmd)  
		child.expect (prompt, timeout=180, searchwindowsize=100) 
		time.sleep(60)

	cmd = "END of test %s" % (testname)
	f.write(cmd)
	f.close   

if __name__ == "__main__":
	main()
