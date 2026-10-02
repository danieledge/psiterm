# tools/emu/seed.py on|off - adds or removes PsiMail's TESTSEED block (an
# offline Example account, mail copied from the card's SEED folders). Emulator
# only: it must never be committed (grep TESTSEED mail/app/psimail.cpp).
import sys
import os
p=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'mail', 'app', 'psimail.cpp'); s=open(p).read()
SEED='''	// TESTSEED (emulator only - remove before release)
	if (!iSettings.iAccounts[0].used)
		{
		PmAccount& ta = iSettings.iAccounts[0];
		ta.used = 1;
		Mem::Copy(ta.name, "Example", 8);
		Mem::Copy(ta.email, "dan@example.com", 16);
		Mem::Copy(ta.user, "dan@example.com", 16);
		Mem::Copy(ta.imap_host, "imap.example.com", 17);
		iSettings.iOffline = 1;
		}
	iSettings.iStore = 1;                    // on C: (the emulator's card wedges on many writes)
	{
	CFileMan* fm = CFileMan::NewL(iCoeEnv->FsSession());
	iCoeEnv->FsSession().MkDirAll(_L("C:\\\\PsiMail\\\\A0\\\\F4A1E411B\\\\"));
	fm->Copy(_L("D:\\\\SEED\\\\*.*"), _L("C:\\\\PsiMail\\\\A0\\\\F4A1E411B\\\\"));
	fm->Copy(_L("D:\\\\SEEDA0\\\\*.*"), _L("C:\\\\PsiMail\\\\A0\\\\"));
	delete fm;
	}
	// /TESTSEED
'''
anchor='	TRAPD(pics, ToolbarPicturesL());'
if sys.argv[1]=='on':
    assert 'TESTSEED' not in s; s=s.replace(anchor, SEED+anchor,1)
else:
    a=s.index('	// TESTSEED'); b=s.index('	// /TESTSEED\n')+len('	// /TESTSEED\n'); s=s[:a]+s[b:]
open(p,'w').write(s)
