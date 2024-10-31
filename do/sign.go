package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

func runCmdLoggedRedacted(cmd *exec.Cmd, redact string) error {
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	s := cmd.String()
	s = strings.ReplaceAll(s, redact, "***")
	fmt.Printf("> %s\n", s)
	return cmd.Run()
}

func runCmdLogged(cmd *exec.Cmd) error {
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	s := cmd.String()
	fmt.Printf("> %s\n", s)
	return cmd.Run()
}

// https://zabkat.com/blog/code-signing-sha1-armageddon.htm
// signtool sign /n "subject name" /t http://timestamp.comodoca.com/authenticode myInstaller.exe
// signtool sign /n "subject name" /fd sha256 /tr http://timestamp.comodoca.com/rfc3161 /td sha256 /as myInstaller.exe
// signtool args (https://msdn.microsoft.com/en-us/library/windows/desktop/aa387764(v=vs.85).aspx):
//
//	/as          : append signature
//	/fd ${alg}   : specify digest algo, default is sha1, SHA256 is recommended
//	/t ${url}    : timestamp server
//	/tr ${url}   : timestamp rfc 3161 server
//	/td ${alg}   : for /tr, must be after /tr
//	/du ${url}   : URL for expanded description of the signed content.
//	/debug       : show debugging info
func signMust(path string) {
	// the sign tool is finicky, so copy the cert to the same dir as
	// the exe we're signing

	// retry 3 times because signing might fail due to temorary error
	// ("The specified timestamp server either could not be reached or")
	var err error
	for i := 0; i < 3; i++ {
		signtoolPath := detectSigntoolPath()
		fileDir := filepath.Dir(path)
		fileName := filepath.Base(path)
		//signServer := "http://timestamp.verisign.com/scripts/timstamp.dll"
		//signServer := "http://timestamp.sectigo.com"
		signServer := "http://time.certum.pl/"
		//desc := "https://www.sumatrapdfreader.org"
		{
			// sign with sha1 for pre-win-7
			// TODO: remove it? We no longer support pre-win7
			cmd := exec.Command(signtoolPath, "sign", "/t", signServer,
				"/n", "Open Source Developer, Krzysztof Kowalczyk",
				//"/du", desc,
				"/fd", "sha256",
				fileName)
			cmd.Dir = fileDir
			err = runCmdLogged(cmd)
		}

		if false && err == nil {
			certPwd := ""
			// double-sign with sha2 for win7+ ater Jan 2016
			cmd := exec.Command(signtoolPath, "sign", "/fd", "sha256", "/tr", signServer,
				"/td", "sha256", "/f", "cert.pfx",
				"/p", certPwd, "/as", fileName)
			cmd.Dir = fileDir
			err = runCmdLoggedRedacted(cmd, certPwd)
		}

		if err == nil {
			return
		}
		time.Sleep(time.Second * 15)
	}
	must(err)
}
