"""
gensuitemodule - Generate an AE suite module from an aete/aeut resource

Based on aete.py.

Reading and understanding this code is left as an exercise to the reader.
"""

import MacOS
import EasyDialogs
import os
import string
import sys
import types
import StringIO
import keyword
import macresource
import aetools
import distutils.sysconfig
import OSATerminology
from Carbon.Res import *
import MacOS
import getopt

_MAC_LIB_FOLDER=os.path.dirname(aetools.__file__)
DEFAULT_STANDARD_PACKAGEFOLDER=os.path.join(_MAC_LIB_FOLDER, 'lib-scriptpackages')
DEFAULT_USER_PACKAGEFOLDER=distutils.sysconfig.get_python_lib()

def usage():
	sys.stderr.write("Usage: %s [opts] application-or-resource-file\n" % sys.argv[0])
	sys.stderr.write("""Options:
--output pkgdir  Pathname of the output package (short: -o)  
--resource       Parse resource file in stead of launching application (-r)
--base package   Use another base package in stead of default StdSuites (-b)
--edit old=new   Edit suite names, use empty new to skip a suite (-e)
--creator code   Set creator code for package (-c)
""")
	sys.exit(1)

def main():
	if len(sys.argv) > 1:
		SHORTOPTS = "rb:o:e:c:"
		LONGOPTS = ("resource", "base=", "output=", "edit=", "creator=")
		try:
			opts, args = getopt.getopt(sys.argv[1:], SHORTOPTS, LONGOPTS)
		except getopt.GetoptError:
			usage()
		
		process_func = processfile
		basepkgname = 'StdSuites'
		output = None
		edit_modnames = []
		creatorsignature = None
		
		for o, a in opts:
			if o in ('-r', '--resource'):
				process_func = processfile_fromresource
			if o in ('-b', '--base'):
				basepkgname = a
			if o in ('-o', '--output'):
				output = a
			if o in ('-e', '--edit'):
				split = a.split('=')
				if len(split) != 2:
					usage()
				edit_modnames.append(split)
			if o in ('-c', '--creator'):
				if len(a) != 4:
					sys.stderr.write("creator must be 4-char string\n")
					sys.exit(1)
				creatorsignature = a
				
					
		if output and len(args) > 1:
			sys.stderr.write("%s: cannot specify --output with multiple inputs\n" % sys.argv[0])
			sys.exit(1)
			
		for filename in args:
			process_func(filename, output=output, basepkgname=basepkgname, 
				edit_modnames=edit_modnames, creatorsignature=creatorsignature)
	else:
		# The dialogOptionFlags below allows selection of .app bundles.
		filename = EasyDialogs.AskFileForOpen(
			message='Select scriptable application',
			dialogOptionFlags=0x1056)
		if not filename:
			sys.exit(0)
		try:
			processfile(filename)
		except MacOS.Error, arg:
			print "Error getting terminology:", arg
			print "Retry, manually parsing resources"
			processfile_fromresource(filename)

def processfile_fromresource(fullname, output=None, basepkgname=None, 
		edit_modnames=None, creatorsignature=None):
	"""Process all resources in a single file"""
	cur = CurResFile()
	print "Processing", fullname
	rf = macresource.open_pathname(fullname)
	try:
		UseResFile(rf)
		resources = []
		for i in range(Count1Resources('aete')):
			res = Get1IndResource('aete', 1+i)
			resources.append(res)
		for i in range(Count1Resources('aeut')):
			res = Get1IndResource('aeut', 1+i)
			resources.append(res)
		print "\nLISTING aete+aeut RESOURCES IN", `fullname`
		aetelist = []
		for res in resources:
			print "decoding", res.GetResInfo(), "..."
			data = res.data
			aete = decode(data)
			aetelist.append((aete, res.GetResInfo()))
	finally:
		if rf <> cur:
			CloseResFile(rf)
			UseResFile(cur)
	# switch back (needed for dialogs in Python)
	UseResFile(cur)
	compileaetelist(aetelist, fullname, output=output, 
		basepkgname=basepkgname, edit_modnames=edit_modnames,
		creatorsignature=creatorsignature)

def processfile(fullname, output=None, basepkgname=None, 
		edit_modnames=None, creatorsignature=None):
	"""Ask an application for its terminology and process that"""
	print "\nASKING FOR aete DICTIONARY IN", `fullname`
	try:
		aedescobj, launched = OSATerminology.GetAppTerminology(fullname)
	except MacOS.Error, arg:
		if arg[0] in (-1701, -192): # errAEDescNotFound, resNotFound
			print "GetAppTerminology failed with errAEDescNotFound/resNotFound, trying manually"
			aedata, sig = getappterminology(fullname)
			if not creatorsignature:
				creatorsignature = sig
		else:
			raise
	else:
		if launched:
			print "Launched", fullname
		raw = aetools.unpack(aedescobj)
		if not raw: 
			print 'Unpack returned empty value:', raw
			return
		if not raw[0].data: 
			print 'Unpack returned value without data:', raw
			return
		aedata = raw[0]
	aete = decode(aedata.data)
	compileaete(aete, None, fullname, output=output, basepkgname=basepkgname,
		creatorsignature=creatorsignature, edit_modnames=edit_modnames)
		
def getappterminology(fullname):
	"""Get application terminology by sending an AppleEvent"""
	# First check that we actually can send AppleEvents
	if not MacOS.WMAvailable():
		raise RuntimeError, "Cannot send AppleEvents, no access to window manager"
	# Next, a workaround for a bug in MacOS 10.2: sending events will hang unless
	# you have created an event loop first.
	import Carbon.Evt
	Carbon.Evt.WaitNextEvent(0,0)
	if os.path.isdir(fullname):
		# Now get the signature of the application, hoping it is a bundle
		pkginfo = os.path.join(fullname, 'Contents', 'PkgInfo')
		if not os.path.exists(pkginfo):
			raise RuntimeError, "No PkgInfo file found"
		tp_cr = open(pkginfo, 'rb').read()
		cr = tp_cr[4:8]
	else:
		# Assume it is a file
		cr, tp = MacOS.GetCreatorAndType(fullname)
	# Let's talk to it and ask for its AETE
	talker = aetools.TalkTo(cr)
	try:
		talker._start()
	except (MacOS.Error, aetools.Error), arg:
		print 'Warning: start() failed, continuing anyway:', arg
	reply = talker.send("ascr", "gdte")
	#reply2 = talker.send("ascr", "gdut")
	# Now pick the bits out of the return that we need.
	return reply[1]['----'], cr
	

def compileaetelist(aetelist, fullname, output=None, basepkgname=None, 
			edit_modnames=None, creatorsignature=None):
	for aete, resinfo in aetelist:
		compileaete(aete, resinfo, fullname, output=output, 
			basepkgname=basepkgname, edit_modnames=edit_modnames,
			creatorsignature=creatorsignature)
		
def decode(data):
	"""Decode a resource into a python data structure"""
	f = StringIO.StringIO(data)
	aete = generic(getaete, f)
	aete = simplify(aete)
	processed = f.tell()
	unprocessed = len(f.read())
	total = f.tell()
	if unprocessed:
		sys.stderr.write("%d processed + %d unprocessed = %d total\n" %
		                 (processed, unprocessed, total))
	return aete

def simplify(item):
	"""Recursively replace singleton tuples by their constituent item"""
	if type(item) is types.ListType:
		return map(simplify, item)
	elif type(item) == types.TupleType and len(item) == 2:
		return simplify(item[1])
	else:
		return item


# Here follows the aete resource decoder.
# It is presented bottom-up instead of top-down because there are  direct
# references to the lower-level part-decoders from the high-level part-decoders.

def getbyte(f, *args):
	c = f.read(1)
	if not c:
		raise EOFError, 'in getbyte' + str(args)
	return ord(c)

def getword(f, *args):
	getalign(f)
	s = f.read(2)
	if len(s) < 2:
		raise EOFError, 'in getword' + str(args)
	return (ord(s[0])<<8) | ord(s[1])

def getlong(f, *args):
	getalign(f)
	s = f.read(4)
	if len(s) < 4:
		raise EOFError, 'in getlong' + str(args)
	return (ord(s[0])<<24) | (ord(s[1])<<16) | (ord(s[2])<<8) | ord(s[3])

def getostype(f, *args):
	getalign(f)
	s = f.read(4)
	if len(s) < 4:
		raise EOFError, 'in getostype' + str(args)
	return s

def getpstr(f, *args):
	c = f.read(1)
	if len(c) < 1:
		raise EOFError, 'in getpstr[1]' + str(args)
	nbytes = ord(c)
	if nbytes == 0: return ''
	s = f.read(nbytes)
	if len(s) < nbytes:
		raise EOFError, 'in getpstr[2]' + str(args)
	return s

def getalign(f):
	if f.tell() & 1:
		c = f.read(1)
		##if c <> '\0':
		##	print 'align:', `c`

def getlist(f, description, getitem):
	count = getword(f)
	list = []
	for i in range(count):
		list.append(generic(getitem, f))
		getalign(f)
	return list

def alt_generic(what, f, *args):
	print "generic", `what`, args
	res = vageneric(what, f, args)
	print '->', `res`
	return res

def generic(what, f, *args):
	if type(what) == types.FunctionType:
		return apply(what, (f,) + args)
	if type(what) == types.ListType:
		record = []
		for thing in what:
			item = apply(generic, thing[:1] + (f,) + thing[1:])
			record.append((thing[1], item))
		return record
	return "BAD GENERIC ARGS: %s" % `what`

getdata = [
	(getostype, "type"),
	(getpstr, "description"),
	(getword, "flags")
	]
getargument = [
	(getpstr, "name"),
	(getostype, "keyword"),
	(getdata, "what")
	]
getevent = [
	(getpstr, "name"),
	(getpstr, "description"),
	(getostype, "suite code"),
	(getostype, "event code"),
	(getdata, "returns"),
	(getdata, "accepts"),
	(getlist, "optional arguments", getargument)
	]
getproperty = [
	(getpstr, "name"),
	(getostype, "code"),
	(getdata, "what")
	]
getelement = [
	(getostype, "type"),
	(getlist, "keyform", getostype)
	]
getclass = [
	(getpstr, "name"),
	(getostype, "class code"),
	(getpstr, "description"),
	(getlist, "properties", getproperty),
	(getlist, "elements", getelement)
	]
getcomparison = [
	(getpstr, "operator name"),
	(getostype, "operator ID"),
	(getpstr, "operator comment"),
	]
getenumerator = [
	(getpstr, "enumerator name"),
	(getostype, "enumerator ID"),
	(getpstr, "enumerator comment")
	]
getenumeration = [
	(getostype, "enumeration ID"),
	(getlist, "enumerator", getenumerator)
	]
getsuite = [
	(getpstr, "suite name"),
	(getpstr, "suite description"),
	(getostype, "suite ID"),
	(getword, "suite level"),
	(getword, "suite version"),
	(getlist, "events", getevent),
	(getlist, "classes", getclass),
	(getlist, "comparisons", getcomparison),
	(getlist, "enumerations", getenumeration)
	]
getaete = [
	(getword, "major/minor version in BCD"),
	(getword, "language code"),
	(getword, "script code"),
	(getlist, "suites", getsuite)
	]

def compileaete(aete, resinfo, fname, output=None, basepkgname=None, 
		edit_modnames=None, creatorsignature=None):
	"""Generate code for a full aete resource. fname passed for doc purposes"""
	[version, language, script, suites] = aete
	major, minor = divmod(version, 256)
	if not creatorsignature:
		creatorsignature, dummy = MacOS.GetCreatorAndType(fname)
	packagename = identify(os.path.splitext(os.path.basename(fname))[0])
	if language:
		packagename = packagename+'_lang%d'%language
	if script:
		packagename = packagename+'_script%d'%script
	if len(packagename) > 27:
		packagename = packagename[:27]
	if output:
		# XXXX Put this in site-packages if it isn't a full pathname?
		if not os.path.exists(output):
			os.mkdir(output)
		pathname = output
	else:
		pathname = EasyDialogs.AskFolder(message='Create and select package folder for %s'%packagename,
			defaultLocation=DEFAULT_USER_PACKAGEFOLDER)
		output = pathname
	if not pathname:
		return
	packagename = os.path.split(os.path.normpath(pathname))[1]
	if not basepkgname:
		basepkgname = EasyDialogs.AskFolder(message='Package folder for base suite (usually StdSuites)',
			defaultLocation=DEFAULT_STANDARD_PACKAGEFOLDER)
	if basepkgname:
		dirname, basepkgname = os.path.split(os.path.normpath(basepkgname))
		if dirname and not dirname in sys.path:
			sys.path.insert(0, dirname)
		basepackage = __import__(basepkgname)
	else:
		basepackage = None
	suitelist = []
	allprecompinfo = []
	allsuites = []
	for suite in suites:
		code, suite, pathname, modname, precompinfo = precompilesuite(suite, basepackage, 
				output=output, edit_modnames=edit_modnames)
		if not code:
			continue
		allprecompinfo = allprecompinfo + precompinfo
		suiteinfo = suite, pathname, modname
		suitelist.append((code, modname))
		allsuites.append(suiteinfo)
	for suiteinfo in allsuites:
		compilesuite(suiteinfo, major, minor, language, script, fname, basepackage, 
				allprecompinfo, interact=(edit_modnames is None))
	initfilename = os.path.join(output, '__init__.py')
	fp = open(initfilename, 'w')
	MacOS.SetCreatorAndType(initfilename, 'Pyth', 'TEXT')
	fp.write('"""\n')
	fp.write("Package generated from %s\n"%fname)
	if resinfo:
		fp.write("Resource %s resid %d %s\n"%(ascii(resinfo[1]), resinfo[0], ascii(resinfo[2])))
	fp.write('"""\n')
	fp.write('import aetools\n')
	fp.write('Error = aetools.Error\n')
	suitelist.sort()
	for code, modname in suitelist:
		fp.write("import %s\n" % modname)
	fp.write("\n\n_code_to_module = {\n")
	for code, modname in suitelist:
		fp.write("\t'%s' : %s,\n"%(ascii(code), modname))
	fp.write("}\n\n")
	fp.write("\n\n_code_to_fullname = {\n")
	for code, modname in suitelist:
		fp.write("\t'%s' : ('%s.%s', '%s'),\n"%(ascii(code), packagename, modname, modname))
	fp.write("}\n\n")
	for code, modname in suitelist:
		fp.write("from %s import *\n"%modname)
	
	# Generate property dicts and element dicts for all types declared in this module
	fp.write("\ndef getbaseclasses(v):\n")
	fp.write("\tif not getattr(v, '_propdict', None):\n")
	fp.write("\t\tv._propdict = {}\n")
	fp.write("\t\tv._elemdict = {}\n")
	fp.write("\t\tfor superclassname in getattr(v, '_superclassnames', []):\n")
	fp.write("\t\t\tsuperclass = eval(superclassname)\n")
	fp.write("\t\t\tgetbaseclasses(superclass)\n")
	fp.write("\t\t\tv._propdict.update(getattr(superclass, '_propdict', {}))\n")
	fp.write("\t\t\tv._elemdict.update(getattr(superclass, '_elemdict', {}))\n")
	fp.write("\t\tv._propdict.update(getattr(v, '_privpropdict', {}))\n")
	fp.write("\t\tv._elemdict.update(getattr(v, '_privelemdict', {}))\n")
	fp.write("\n")
	fp.write("import StdSuites\n")
	allprecompinfo.sort()
	if allprecompinfo:
		fp.write("\n#\n# Set property and element dictionaries now that all classes have been defined\n#\n")
		for codenamemapper in allprecompinfo:
			for k, v in codenamemapper.getall('class'):
				fp.write("getbaseclasses(%s)\n" % v)

	# Generate a code-to-name mapper for all of the types (classes) declared in this module
	if allprecompinfo:
		fp.write("\n#\n# Indices of types declared in this module\n#\n")
		fp.write("_classdeclarations = {\n")
		for codenamemapper in allprecompinfo:
			for k, v in codenamemapper.getall('class'):
				fp.write("\t%s : %s,\n" % (`k`, v))
		fp.write("}\n")

	if suitelist:
		fp.write("\n\nclass %s(%s_Events"%(packagename, suitelist[0][1]))
		for code, modname in suitelist[1:]:
			fp.write(",\n\t\t%s_Events"%modname)
		fp.write(",\n\t\taetools.TalkTo):\n")
		fp.write("\t_signature = %s\n\n"%`creatorsignature`)
		fp.write("\t_moduleName = '%s'\n\n"%packagename)
	fp.close()
	
def precompilesuite(suite, basepackage=None, edit_modnames=None, output=None):
	"""Parse a single suite without generating the output. This step is needed
	so we can resolve recursive references by suites to enums/comps/etc declared
	in other suites"""
	[name, desc, code, level, version, events, classes, comps, enums] = suite
	
	modname = identify(name)
	if len(modname) > 28:
		modname = modname[:27]
	if edit_modnames is None:
		pathname = EasyDialogs.AskFileForSave(message='Python output file',
			savedFileName=modname+'.py')
	else:
		for old, new in edit_modnames:
			if old == modname:
				modname = new
		if modname:
			pathname = os.path.join(output, modname + '.py')
		else:
			pathname = None
	if not pathname:
		return None, None, None, None, None

	modname = os.path.splitext(os.path.split(pathname)[1])[0]
	
	if basepackage and basepackage._code_to_module.has_key(code):
		# We are an extension of a baseclass (usually an application extending
		# Standard_Suite or so). Import everything from our base module
		basemodule = basepackage._code_to_module[code]
	else:
		# We are not an extension.
		basemodule = None

	enumsneeded = {}
	for event in events:
		findenumsinevent(event, enumsneeded)

	objc = ObjectCompiler(None, basemodule, interact=(edit_modnames is None))
	for cls in classes:
		objc.compileclass(cls)
	for cls in classes:
		objc.fillclasspropsandelems(cls)
	for comp in comps:
		objc.compilecomparison(comp)
	for enum in enums:
		objc.compileenumeration(enum)
	
	for enum in enumsneeded.keys():
		objc.checkforenum(enum)
		
	objc.dumpindex()
	
	precompinfo = objc.getprecompinfo(modname)
	
	return code, suite, pathname, modname, precompinfo

def compilesuite((suite, pathname, modname), major, minor, language, script, 
		fname, basepackage, precompinfo, interact=1):
	"""Generate code for a single suite"""
	[name, desc, code, level, version, events, classes, comps, enums] = suite
	# Sort various lists, so re-generated source is easier compared
	events.sort()
	classes.sort()
	comps.sort()
	enums.sort()
	
	fp = open(pathname, 'w')
	MacOS.SetCreatorAndType(pathname, 'Pyth', 'TEXT')
	
	fp.write('"""Suite %s: %s\n' % (ascii(name), ascii(desc)))
	fp.write("Level %d, version %d\n\n" % (level, version))
	fp.write("Generated from %s\n"%ascii(fname))
	fp.write("AETE/AEUT resource version %d/%d, language %d, script %d\n" % \
		(major, minor, language, script))
	fp.write('"""\n\n')
	
	fp.write('import aetools\n')
	fp.write('import MacOS\n\n')
	fp.write("_code = %s\n\n"% `code`)
	if basepackage and basepackage._code_to_module.has_key(code):
		# We are an extension of a baseclass (usually an application extending
		# Standard_Suite or so). Import everything from our base module
		fp.write('from %s import *\n'%basepackage._code_to_fullname[code][0])
		basemodule = basepackage._code_to_module[code]
	elif basepackage and basepackage._code_to_module.has_key(code.lower()):
		# This is needed by CodeWarrior and some others.
		fp.write('from %s import *\n'%basepackage._code_to_fullname[code.lower()][0])
		basemodule = basepackage._code_to_module[code.lower()]
	else:
		# We are not an extension.
		basemodule = None
	compileclassheader(fp, modname, basemodule)

	enumsneeded = {}
	if events:
		for event in events:
			compileevent(fp, event, enumsneeded)
	else:
		fp.write("\tpass\n\n")

	objc = ObjectCompiler(fp, basemodule, precompinfo, interact=interact)
	for cls in classes:
		objc.compileclass(cls)
	for cls in classes:
		objc.fillclasspropsandelems(cls)
	for comp in comps:
		objc.compilecomparison(comp)
	for enum in enums:
		objc.compileenumeration(enum)
	
	for enum in enumsneeded.keys():
		objc.checkforenum(enum)
		
	objc.dumpindex()
	
	return code, modname

def compileclassheader(fp, name, module=None):
	"""Generate class boilerplate"""
	classname = '%s_Events'%name
	if module:
		modshortname = string.split(module.__name__, '.')[-1]
		baseclassname = '%s_Events'%modshortname
		fp.write("class %s(%s):\n\n"%(classname, baseclassname))
	else:
		fp.write("class %s:\n\n"%classname)
	
def compileevent(fp, event, enumsneeded):
	"""Generate code for a single event"""
	[name, desc, code, subcode, returns, accepts, arguments] = event
	funcname = identify(name)
	#
	# generate name->keyword map
	#
	if arguments:
		fp.write("\t_argmap_%s = {\n"%funcname)
		for a in arguments:
			fp.write("\t\t%s : %s,\n"%(`identify(a[0])`, `a[1]`))
		fp.write("\t}\n\n")
		
	#
	# Generate function header
	#
	has_arg = (not is_null(accepts))
	opt_arg = (has_arg and is_optional(accepts))
	
	fp.write("\tdef %s(self, "%funcname)
	if has_arg:
		if not opt_arg:
			fp.write("_object, ")		# Include direct object, if it has one
		else:
			fp.write("_object=None, ")	# Also include if it is optional
	else:
		fp.write("_no_object=None, ")	# For argument checking
	fp.write("_attributes={}, **_arguments):\n")	# include attribute dict and args
	#
	# Generate doc string (important, since it may be the only
	# available documentation, due to our name-remaping)
	#
	fp.write('\t\t"""%s: %s\n'%(ascii(name), ascii(desc)))
	if has_arg:
		fp.write("\t\tRequired argument: %s\n"%getdatadoc(accepts))
	elif opt_arg:
		fp.write("\t\tOptional argument: %s\n"%getdatadoc(accepts))
	for arg in arguments:
		fp.write("\t\tKeyword argument %s: %s\n"%(identify(arg[0]),
				getdatadoc(arg[2])))
	fp.write("\t\tKeyword argument _attributes: AppleEvent attribute dictionary\n")
	if not is_null(returns):
		fp.write("\t\tReturns: %s\n"%getdatadoc(returns))
	fp.write('\t\t"""\n')
	#
	# Fiddle the args so everything ends up in 'arguments' dictionary
	#
	fp.write("\t\t_code = %s\n"% `code`)
	fp.write("\t\t_subcode = %s\n\n"% `subcode`)
	#
	# Do keyword name substitution
	#
	if arguments:
		fp.write("\t\taetools.keysubst(_arguments, self._argmap_%s)\n"%funcname)
	else:
		fp.write("\t\tif _arguments: raise TypeError, 'No optional args expected'\n")
	#
	# Stuff required arg (if there is one) into arguments
	#
	if has_arg:
		fp.write("\t\t_arguments['----'] = _object\n")
	elif opt_arg:
		fp.write("\t\tif _object:\n")
		fp.write("\t\t\t_arguments['----'] = _object\n")
	else:
		fp.write("\t\tif _no_object != None: raise TypeError, 'No direct arg expected'\n")
	fp.write("\n")
	#
	# Do enum-name substitution
	#
	for a in arguments:
		if is_enum(a[2]):
			kname = a[1]
			ename = a[2][0]
			if ename <> '****':
				fp.write("\t\taetools.enumsubst(_arguments, %s, _Enum_%s)\n" %
					(`kname`, identify(ename)))
				enumsneeded[ename] = 1
	fp.write("\n")
	#
	# Do the transaction
	#
	fp.write("\t\t_reply, _arguments, _attributes = self.send(_code, _subcode,\n")
	fp.write("\t\t\t\t_arguments, _attributes)\n")
	#
	# Error handling
	#
	fp.write("\t\tif _arguments.get('errn', 0):\n")
	fp.write("\t\t\traise aetools.Error, aetools.decodeerror(_arguments)\n")
	fp.write("\t\t# XXXX Optionally decode result\n")
	#
	# Decode result
	#
	fp.write("\t\tif _arguments.has_key('----'):\n")
	if is_enum(returns):
		fp.write("\t\t\t# XXXX Should do enum remapping here...\n")
	fp.write("\t\t\treturn _arguments['----']\n")
	fp.write("\n")
	
#	print "\n#    Command %s -- %s (%s, %s)" % (`name`, `desc`, `code`, `subcode`)
#	print "#        returns", compiledata(returns)
#	print "#        accepts", compiledata(accepts)
#	for arg in arguments:
#		compileargument(arg)

def compileargument(arg):
	[name, keyword, what] = arg
	print "#        %s (%s)" % (name, `keyword`), compiledata(what)

def findenumsinevent(event, enumsneeded):
	"""Find all enums for a single event"""
	[name, desc, code, subcode, returns, accepts, arguments] = event
	for a in arguments:
		if is_enum(a[2]):
			ename = a[2][0]
			if ename <> '****':
				enumsneeded[ename] = 1

#
# This class stores the code<->name translations for a single module. It is used
# to keep the information while we're compiling the module, but we also keep these objects
# around so if one suite refers to, say, an enum in another suite we know where to
# find it. Finally, if we really can't find a code, the user can add modules by
# hand.
#
class CodeNameMapper:
	
	def __init__(self, interact=1):
		self.code2name = {
			"property" : {},
			"class" : {},
			"enum" : {},
			"comparison" : {},
		}
		self.name2code =  {
			"property" : {},
			"class" : {},
			"enum" : {},
			"comparison" : {},
		}
		self.modulename = None
		self.star_imported = 0
		self.can_interact = interact
		
	def addnamecode(self, type, name, code):
		self.name2code[type][name] = code
		if not self.code2name[type].has_key(code):
			self.code2name[type][code] = name
		
	def hasname(self, type, name):
		return self.name2code[type].has_key(name)
		
	def hascode(self, type, code):
		return self.code2name[type].has_key(code)
		
	def findcodename(self, type, code):
		if not self.hascode(type, code):
			return None, None, None
		name = self.code2name[type][code]
		if self.modulename and not self.star_imported:
			qualname = '%s.%s'%(self.modulename, name)
		else:
			qualname = name
		return name, qualname, self.modulename
		
	def getall(self, type):
		return self.code2name[type].items()
			
	def addmodule(self, module, name, star_imported):
		self.modulename = name
		self.star_imported = star_imported
		for code, name in module._propdeclarations.items():
			self.addnamecode('property', name, code)
		for code, name in module._classdeclarations.items():
			self.addnamecode('class', name, code)
		for code in module._enumdeclarations.keys():
			self.addnamecode('enum', '_Enum_'+identify(code), code)
		for code, name in module._compdeclarations.items():
			self.addnamecode('comparison', name, code)
		
	def prepareforexport(self, name=None):
		if not self.modulename:
			self.modulename = name
		return self
			
class ObjectCompiler:
	def __init__(self, fp, basesuite=None, othernamemappers=None, interact=1):
		self.fp = fp
		self.basesuite = basesuite
		self.can_interact = interact
		self.namemappers = [CodeNameMapper(self.can_interact)]
		if othernamemappers:
			self.othernamemappers = othernamemappers[:]
		else:
			self.othernamemappers = []
		if basesuite:
			basemapper = CodeNameMapper(self.can_interact)
			basemapper.addmodule(basesuite, '', 1)
			self.namemappers.append(basemapper)
		
	def getprecompinfo(self, modname):
		list = []
		for mapper in self.namemappers:
			emapper = mapper.prepareforexport(modname)
			if emapper:
				list.append(emapper)
		return list
		
	def findcodename(self, type, code):
		while 1:
			# First try: check whether we already know about this code.
			for mapper in self.namemappers:
				if mapper.hascode(type, code):
					return mapper.findcodename(type, code)
			# Second try: maybe one of the other modules knows about it.
			for mapper in self.othernamemappers:
				if mapper.hascode(type, code):
					self.othernamemappers.remove(mapper)
					self.namemappers.append(mapper)
					if self.fp:
						self.fp.write("import %s\n"%mapper.modulename)
					break
			else:
				# If all this has failed we ask the user for a guess on where it could
				# be and retry.
				if self.fp:
					m = self.askdefinitionmodule(type, code)
				else:
					m = None
				if not m: return None, None, None
				mapper = CodeNameMapper(self.can_interact)
				mapper.addmodule(m, m.__name__, 0)
				self.namemappers.append(mapper)
	
	def askdefinitionmodule(self, type, code):
		if not self.can_interact:
			print "** No definition for %s '%s' found" % (type, code)
			return None
		path = EasyDialogs.AskFileForSave(message='Where is %s %s declared?'%(type, code))
		if not path: return
		path, file = os.path.split(path)
		modname = os.path.splitext(file)[0]
		if not path in sys.path:
			sys.path.insert(0, path)
		m = __import__(modname)
		self.fp.write("import %s\n"%modname)
		return m
		
	def compileclass(self, cls):
		[name, code, desc, properties, elements] = cls
		pname = identify(name)
		if self.namemappers[0].hascode('class', code):
			# plural forms and such
			othername, dummy, dummy = self.namemappers[0].findcodename('class', code)
			if self.fp:
				self.fp.write("\n%s = %s\n"%(pname, othername))
		else:
			if self.fp:
				self.fp.write('\nclass %s(aetools.ComponentItem):\n' % pname)
				self.fp.write('\t"""%s - %s """\n' % (ascii(name), ascii(desc)))
				self.fp.write('\twant = %s\n' % `code`)
		self.namemappers[0].addnamecode('class', pname, code)
		properties.sort()
		for prop in properties:
			self.compileproperty(prop)
		elements.sort()
		for elem in elements:
			self.compileelement(elem)
	
	def compileproperty(self, prop):
		[name, code, what] = prop
		if code == 'c@#!':
			# Something silly with plurals. Skip it.
			return
		pname = identify(name)
		if self.namemappers[0].hascode('property', code):
			# plural forms and such
			othername, dummy, dummy = self.namemappers[0].findcodename('property', code)
			if pname == othername:
				return
			if self.fp:
				self.fp.write("\n%s = %s\n"%(pname, othername))
		else:
			if self.fp:
				self.fp.write("class %s(aetools.NProperty):\n" % pname)
				self.fp.write('\t"""%s - %s """\n' % (ascii(name), ascii(what[1])))
				self.fp.write("\twhich = %s\n" % `code`)
				self.fp.write("\twant = %s\n" % `what[0]`)
		self.namemappers[0].addnamecode('property', pname, code)
	
	def compileelement(self, elem):
		[code, keyform] = elem
		if self.fp:
			self.fp.write("#        element %s as %s\n" % (`code`, keyform))

	def fillclasspropsandelems(self, cls):
		[name, code, desc, properties, elements] = cls
		cname = identify(name)
		if self.namemappers[0].hascode('class', code) and \
				self.namemappers[0].findcodename('class', code)[0] != cname:
			# This is an other name (plural or so) for something else. Skip.
			return
		plist = []
		elist = []
		superclasses = []
		for prop in properties:
			[pname, pcode, what] = prop
			if pcode == "c@#^":
				superclasses.append(what)
			if pcode == 'c@#!':
				continue
			pname = identify(pname)
			plist.append(pname)

		superclassnames = []
		for superclass in superclasses:
			superId, superDesc, dummy = superclass
			superclassname, fullyqualifiedname, module = self.findcodename("class", superId)
			superclassnames.append(superclassname)

		if self.fp:
			self.fp.write("%s._superclassnames = %s\n"%(cname, `superclassnames`))

		for elem in elements:
			[ecode, keyform] = elem
			if ecode == 'c@#!':
				continue
			name, ename, module = self.findcodename('class', ecode)
			if not name:
				if self.fp:
					self.fp.write("# XXXX %s element %s not found!!\n"%(cname, `ecode`))
			else:
				elist.append((name, ename))
				
		plist.sort()
		elist.sort()
		
		if self.fp:
			self.fp.write("%s._privpropdict = {\n"%cname)
			for n in plist:
				self.fp.write("\t'%s' : %s,\n"%(n, n))
			self.fp.write("}\n")
			self.fp.write("%s._privelemdict = {\n"%cname)
			for n, fulln in elist:
				self.fp.write("\t'%s' : %s,\n"%(n, fulln))
			self.fp.write("}\n")
	
	def compilecomparison(self, comp):
		[name, code, comment] = comp
		iname = identify(name)
		self.namemappers[0].addnamecode('comparison', iname, code)
		if self.fp:
			self.fp.write("class %s(aetools.NComparison):\n" % iname)
			self.fp.write('\t"""%s - %s """\n' % (ascii(name), ascii(comment)))
		
	def compileenumeration(self, enum):
		[code, items] = enum
		name = "_Enum_%s" % identify(code)
		if self.fp:
			self.fp.write("%s = {\n" % name)
			for item in items:
				self.compileenumerator(item)
			self.fp.write("}\n\n")
		self.namemappers[0].addnamecode('enum', name, code)
		return code
	
	def compileenumerator(self, item):
		[name, code, desc] = item
		self.fp.write("\t%s : %s,\t# %s\n" % (`identify(name)`, `code`, ascii(desc)))
		
	def checkforenum(self, enum):
		"""This enum code is used by an event. Make sure it's available"""
		name, fullname, module = self.findcodename('enum', enum)
		if not name:
			if self.fp:
				self.fp.write("_Enum_%s = None # XXXX enum %s not found!!\n"%(identify(enum), ascii(enum)))
			return
		if module:
			if self.fp:
				self.fp.write("from %s import %s\n"%(module, name))
		
	def dumpindex(self):
		if not self.fp:
			return
		self.fp.write("\n#\n# Indices of types declared in this module\n#\n")
		
		self.fp.write("_classdeclarations = {\n")
		classlist = self.namemappers[0].getall('class')
		classlist.sort()
		for k, v in classlist:
			self.fp.write("\t%s : %s,\n" % (`k`, v))
		self.fp.write("}\n")
		
		self.fp.write("\n_propdeclarations = {\n")
		proplist = self.namemappers[0].getall('property')
		proplist.sort()
		for k, v in proplist:
			self.fp.write("\t%s : %s,\n" % (`k`, v))
		self.fp.write("}\n")
		
		self.fp.write("\n_compdeclarations = {\n")
		complist = self.namemappers[0].getall('comparison')
		complist.sort()
		for k, v in complist:
			self.fp.write("\t%s : %s,\n" % (`k`, v))
		self.fp.write("}\n")
		
		self.fp.write("\n_enumdeclarations = {\n")
		enumlist = self.namemappers[0].getall('enum')
		enumlist.sort()
		for k, v in enumlist:
			self.fp.write("\t%s : %s,\n" % (`k`, v))
		self.fp.write("}\n")

def compiledata(data):
	[type, description, flags] = data
	return "%s -- %s %s" % (`type`, `description`, compiledataflags(flags))
	
def is_null(data):
	return data[0] == 'null'
	
def is_optional(data):
	return (data[2] & 0x8000)
	
def is_enum(data):
	return (data[2] & 0x2000)
	
def getdatadoc(data):
	[type, descr, flags] = data
	if descr:
		return ascii(descr)
	if type == '****':
		return 'anything'
	if type == 'obj ':
		return 'an AE object reference'
	return "undocumented, typecode %s"%`type`

dataflagdict = {15: "optional", 14: "list", 13: "enum", 12: "mutable"}
def compiledataflags(flags):
	bits = []
	for i in range(16):
		if flags & (1<<i):
			if i in dataflagdict.keys():
				bits.append(dataflagdict[i])
			else:
				bits.append(`i`)
	return '[%s]' % string.join(bits)
	
def ascii(str):
	"""Return a string with all non-ascii characters hex-encoded"""
	if type(str) != type(''):
		return map(ascii, str)
	rv = ''
	for c in str:
		if c in ('\t', '\n', '\r') or ' ' <= c < chr(0x7f):
			rv = rv + c
		else:
			rv = rv + '\\' + 'x%02.2x' % ord(c)
	return rv
	
def identify(str):
	"""Turn any string into an identifier:
	- replace space by _
	- replace other illegal chars by _xx_ (hex code)
	- prepend _ if the result is a python keyword
	"""
	if not str:
		return "empty_ae_name_"
	rv = ''
	ok = string.ascii_letters + '_'
	ok2 = ok + string.digits
	for c in str:
		if c in ok:
			rv = rv + c
		elif c == ' ':
			rv = rv + '_'
		else:
			rv = rv + '_%02.2x_'%ord(c)
		ok = ok2
	if keyword.iskeyword(rv):
		rv = rv + '_'
	return rv

# Call the main program

if __name__ == '__main__':
	main()
	sys.exit(1)
