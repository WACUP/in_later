CSC = $(DO)csc -nologo -o+ -out:$@ $(if $(filter %.dll,$@),-t:library) $^
NDOC = $(DO)"C:/Program Files (x86)/NDoc3/bin/NDoc3Console.exe"

# no user-configurable paths below this line

ifndef DO
$(error Use "Makefile" instead of "csharp.mk")
endif

csharp: csharp/asap2wav.exe csharp/asapplay.exe
.PHONY: csharp

csharp/asap2wav.exe: $(srcdir)csharp/asap2wav.cs csharp/asap.cs
	$(CSC)
CLEAN += csharp/asap2wav.exe

csharp/asapplay.exe: $(srcdir)csharp/asapplay.cs csharp/asap.cs
	$(CSC)
CLEAN += csharp/asapplay.exe

csharp/asap.cs: $(call src,asap.fu asap6502.fu asapinfo.fu cpu6502.fu pokey.fu) $(ASM6502_PLAYERS_OBX)
	$(FUT) -n Sf.Asap
CLEAN += csharp/asap.cs

csharp/doc/ASAP.chm: csharp/doc/ASAP.dll
	$(NDOC) -documenter:MSDN -CleanIntermediates=true -DocumentInheritedFrameworkMembers=false \
		-OutputDirectory=csharp/doc -OutputTarget=HtmlHelp -HtmlHelpName=ASAP -Title="ASAP .NET API" $<
CLEAN += csharp/doc/ASAP.chm

csharp/doc/ASAP.dll: csharp/asap.cs
	$(CSC) -t:library -doc:csharp/doc/ASAP.xml
CLEAN += csharp/doc/ASAP.dll csharp/doc/ASAP.xml
