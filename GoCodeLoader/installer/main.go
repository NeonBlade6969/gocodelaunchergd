// GoCode installer.
//
// 1. Asks you to locate your Steam Geometry Dash folder (the one containing GeometryDash.exe).
// 2. Gets the GoCode loader: downloads it from loaderURL if one was baked in at build time,
//    otherwise (or if the download fails) uses the "payload" folder next to this exe.
// 3. Installs XInput9_1_0.dll (the loader) and the gocode\ folder into Geometry Dash.
//
// Build:  see ..\build.bat     (go build, no external modules needed)
package main

import (
	"archive/zip"
	"bufio"
	"bytes"
	"crypto/sha256"
	"debug/pe"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"regexp"
	"strings"
	"time"
)

// Set at build time, e.g.  go build -ldflags "-X main.loaderURL=https://example.com/gocode-payload.zip"
var (
	loaderURL    = "" // zip containing XInput9_1_0.dll and gocode\...  (empty = use local payload folder)
	loaderSHA256 = "" // optional hex sha256 of that zip
)

const (
	gdExe     = "GeometryDash.exe"
	proxyName = "XInput9_1_0.dll"
	backupExt = ".gocode-backup"
	marker    = "GOCODE_LOADER_MARKER_v1" // exported by our loader DLL
	maxZip    = 100 << 20
)

var stdin = bufio.NewReader(os.Stdin)

func main() {
	fmt.Println("==============================================")
	fmt.Println("  GoCode - Geometry Dash mod loader installer")
	fmt.Println("==============================================")
	fmt.Println()

	gdDir := chooseGDFolder()
	if gdDir == "" {
		exitWait(1)
	}
	fmt.Println("\nGeometry Dash folder:", gdDir)

	for {
		fmt.Println("\n  1) Install / update GoCode")
		fmt.Println("  2) Uninstall GoCode")
		fmt.Println("  3) Quit")
		switch ask("\nChoose [1-3]: ") {
		case "1", "":
			if err := install(gdDir); err != nil {
				fmt.Println("\nInstall failed:", err)
				exitWait(1)
			}
			exitWait(0)
		case "2":
			if err := uninstall(gdDir); err != nil {
				fmt.Println("\nUninstall failed:", err)
				exitWait(1)
			}
			exitWait(0)
		case "3", "q", "Q":
			return
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Finding the game folder
// ---------------------------------------------------------------------------------------------

func chooseGDFolder() string {
	fmt.Println("Please locate your Steam Geometry Dash folder")
	fmt.Println("(the folder that contains " + gdExe + ").")

	initial := ""
	if found := detectSteamGD(); len(found) > 0 {
		initial = found[0]
		fmt.Println("\nFound a likely install:", initial)
	}
	fmt.Println("A folder window will open - pick the folder and press OK.")

	for {
		dir := pickFolder(initial)
		if dir == "" {
			// Dialog cancelled or unavailable: fall back to typing the path.
			dir = strings.Trim(ask("\nPaste the folder path here (or press Enter to quit): "), "\" ")
			if dir == "" {
				return ""
			}
		}
		if err := validateGD(dir); err != nil {
			fmt.Println("\nThat doesn't look right:", err)
			if ask("Try again? [Y/n]: ") == "n" {
				return ""
			}
			continue
		}
		return dir
	}
}

func validateGD(dir string) error {
	st, err := os.Stat(filepath.Join(dir, gdExe))
	if err != nil || st.IsDir() {
		return fmt.Errorf("%s was not found in %s", gdExe, dir)
	}
	return nil
}

// detectSteamGD looks in the Steam install and every Steam library folder for Geometry Dash.
func detectSteamGD() []string {
	roots := []string{}
	if out, err := exec.Command("reg", "query", `HKCU\Software\Valve\Steam`, "/v", "SteamPath").Output(); err == nil {
		for _, line := range strings.Split(string(out), "\n") {
			if i := strings.Index(line, "REG_SZ"); i >= 0 {
				roots = append(roots, filepath.FromSlash(strings.TrimSpace(line[i+len("REG_SZ"):])))
			}
		}
	}
	roots = append(roots, `C:\Program Files (x86)\Steam`, `C:\Program Files\Steam`)

	libs := []string{}
	libRe := regexp.MustCompile(`"path"\s+"([^"]+)"`)
	for _, r := range roots {
		libs = append(libs, r)
		if data, err := os.ReadFile(filepath.Join(r, "steamapps", "libraryfolders.vdf")); err == nil {
			for _, m := range libRe.FindAllStringSubmatch(string(data), -1) {
				libs = append(libs, strings.ReplaceAll(m[1], `\\`, `\`))
			}
		}
	}

	seen := map[string]bool{}
	found := []string{}
	for _, l := range libs {
		c := filepath.Join(l, "steamapps", "common", "Geometry Dash")
		key := strings.ToLower(c)
		if seen[key] {
			continue
		}
		seen[key] = true
		if validateGD(c) == nil {
			found = append(found, c)
		}
	}
	return found
}

// pickFolder shows the standard Windows folder picker via PowerShell. Returns "" if cancelled/unavailable.
func pickFolder(initial string) string {
	script := `Add-Type -AssemblyName System.Windows.Forms
$d = New-Object System.Windows.Forms.FolderBrowserDialog
$d.Description = 'Select your Steam Geometry Dash folder (contains GeometryDash.exe)'
$d.ShowNewFolderButton = $false
$d.SelectedPath = '` + strings.ReplaceAll(initial, "'", "''") + `'
$f = New-Object System.Windows.Forms.Form
$f.TopMost = $true
if ($d.ShowDialog($f) -eq [System.Windows.Forms.DialogResult]::OK) { Write-Output $d.SelectedPath }`
	script = strings.ReplaceAll(script, "\n", "; ")
	out, err := exec.Command("powershell", "-NoProfile", "-STA", "-Command", script).Output()
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(out))
}

// ---------------------------------------------------------------------------------------------
// Install / uninstall
// ---------------------------------------------------------------------------------------------

func install(gdDir string) error {
	if gdRunning() {
		return errors.New("Geometry Dash is running - close it and try again")
	}

	machine, err := peMachine(filepath.Join(gdDir, gdExe))
	if err != nil {
		return fmt.Errorf("could not read %s: %w", gdExe, err)
	}
	if machine != pe.IMAGE_FILE_MACHINE_I386 {
		return errors.New("this Geometry Dash is not a 32-bit build; this GoCode loader is 32-bit only")
	}

	payload, cleanup, err := getPayload()
	if err != nil {
		return err
	}
	defer cleanup()

	srcProxy := filepath.Join(payload, proxyName)
	dstProxy := filepath.Join(gdDir, proxyName)
	if _, err := os.Stat(srcProxy); err != nil {
		return fmt.Errorf("loader package is missing %s", proxyName)
	}

	// Keep a pre-existing, non-GoCode XInput9_1_0.dll so uninstall can restore it.
	if _, err := os.Stat(dstProxy); err == nil && !isOurs(dstProxy) {
		bak := dstProxy + backupExt
		if _, err := os.Stat(bak); err != nil {
			fmt.Println("Backing up existing", proxyName)
			if err := os.Rename(dstProxy, bak); err != nil {
				return err
			}
		}
	}
	if err := copyFile(srcProxy, dstProxy); err != nil {
		return fmt.Errorf("copying %s: %w", proxyName, err)
	}

	// gocode\ folder (mods, config). Never overwrite files the user may have edited.
	srcRoot := filepath.Join(payload, "gocode")
	dstRoot := filepath.Join(gdDir, "gocode")
	if err := os.MkdirAll(filepath.Join(dstRoot, "mods"), 0o755); err != nil {
		return err
	}
	added := 0
	if _, err := os.Stat(srcRoot); err == nil {
		err = filepath.Walk(srcRoot, func(p string, info os.FileInfo, err error) error {
			if err != nil || info.IsDir() {
				return err
			}
			rel, _ := filepath.Rel(srcRoot, p)
			dst := filepath.Join(dstRoot, rel)
			// Always refresh shipped mods (.gocode, .dll) but never overwrite settings the user may have edited.
			ext := strings.ToLower(filepath.Ext(dst))
			if _, err := os.Stat(dst); err == nil && ext != ".gocode" && ext != ".dll" {
				return nil
			}
			if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
				return err
			}
			added++
			return copyFile(p, dst)
		})
		if err != nil {
			return err
		}
	}

	fmt.Println("\nGoCode installed!")
	fmt.Println("  Loader:  ", dstProxy)
	fmt.Println("  Mods go in:", filepath.Join(dstRoot, "mods"), "(.gocode mods)")
	fmt.Println("  Log file:", filepath.Join(dstRoot, "gocode.log"))
	if added > 0 {
		fmt.Printf("  (%d starter file(s) added)\n", added)
	}
	fmt.Println("\nIn-game keys (game window focused):")
	fmt.Println("  F5 / F6  speed down / up     F7  speed 1.0x")
	fmt.Println("  F8       next FPS preset     Shift+F8  FPS bypass off")
	fmt.Println("Any FPS or speed: put fps=... or speed=... in gocode\\config\\fps-bypass.cfg / speedhack.cfg (applies live).")
	fmt.Println("Safe Mode: using any speed other than 1.0x blocks score/level uploads until you restart the game.")
	fmt.Println("\nLaunch Geometry Dash from Steam as usual.")
	return nil
}

func uninstall(gdDir string) error {
	if gdRunning() {
		return errors.New("Geometry Dash is running - close it and try again")
	}
	dst := filepath.Join(gdDir, proxyName)
	if _, err := os.Stat(dst); err == nil {
		if !isOurs(dst) {
			return fmt.Errorf("%s in this folder is not the GoCode loader; leaving it alone", proxyName)
		}
		if err := os.Remove(dst); err != nil {
			return err
		}
		fmt.Println("Removed", dst)
	} else {
		fmt.Println("GoCode loader was not installed.")
	}
	bak := dst + backupExt
	if _, err := os.Stat(bak); err == nil {
		if err := os.Rename(bak, dst); err != nil {
			return err
		}
		fmt.Println("Restored your original", proxyName)
	}
	fmt.Println("Your gocode\\ folder (mods, log) was left in place - delete it yourself if you want it gone.")
	return nil
}

// ---------------------------------------------------------------------------------------------
// Getting the loader files
// ---------------------------------------------------------------------------------------------

// getPayload returns a directory containing XInput9_1_0.dll and gocode\...
func getPayload() (dir string, cleanup func(), err error) {
	if loaderURL != "" {
		tmp, terr := os.MkdirTemp("", "gocode-")
		if terr == nil {
			fmt.Println("\nDownloading GoCode from", loaderURL)
			derr := downloadAndExtract(loaderURL, tmp)
			if derr == nil {
				return tmp, func() { os.RemoveAll(tmp) }, nil
			}
			os.RemoveAll(tmp)
			fmt.Println("Download failed:", derr)
		}
	}
	local := filepath.Join(exeDir(), "payload")
	if _, serr := os.Stat(filepath.Join(local, proxyName)); serr == nil {
		if loaderURL != "" {
			fmt.Println("Using the local payload folder instead.")
		}
		return local, func() {}, nil
	}
	return "", nil, errors.New("no loader available: no download URL worked and no 'payload' folder sits next to this installer")
}

func downloadAndExtract(url, dest string) error {
	client := &http.Client{Timeout: 90 * time.Second}
	resp, err := client.Get(url)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("server returned %s", resp.Status)
	}
	data, err := io.ReadAll(io.LimitReader(resp.Body, maxZip+1))
	if err != nil {
		return err
	}
	if len(data) > maxZip {
		return errors.New("download is unexpectedly large")
	}
	if loaderSHA256 != "" {
		sum := sha256.Sum256(data)
		if !strings.EqualFold(hex.EncodeToString(sum[:]), loaderSHA256) {
			return errors.New("checksum mismatch - refusing to install")
		}
	}
	fmt.Printf("Downloaded %.1f KB\n", float64(len(data))/1024)

	zr, err := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		return err
	}
	for _, f := range zr.File {
		// Windows PowerShell's Compress-Archive writes backslashes; normalise, then block path escapes.
		name := path.Clean(strings.ReplaceAll(f.Name, `\`, "/"))
		if name == "." || name == ".." || strings.HasPrefix(name, "../") || strings.HasPrefix(name, "/") || strings.Contains(name, ":") {
			continue
		}
		target := filepath.Join(dest, filepath.FromSlash(name))
		if f.FileInfo().IsDir() || strings.HasSuffix(f.Name, "/") || strings.HasSuffix(f.Name, `\`) {
			if err := os.MkdirAll(target, 0o755); err != nil {
				return err
			}
			continue
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return err
		}
		if err := extractOne(f, target); err != nil {
			return err
		}
	}
	return nil
}

func extractOne(f *zip.File, target string) error {
	rc, err := f.Open()
	if err != nil {
		return err
	}
	defer rc.Close()
	out, err := os.Create(target)
	if err != nil {
		return err
	}
	defer out.Close()
	_, err = io.Copy(out, io.LimitReader(rc, maxZip))
	return err
}

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------

func peMachine(file string) (uint16, error) {
	f, err := pe.Open(file)
	if err != nil {
		return 0, err
	}
	defer f.Close()
	return f.Machine, nil
}

func isOurs(file string) bool {
	data, err := os.ReadFile(file)
	return err == nil && bytes.Contains(data, []byte(marker))
}

func gdRunning() bool {
	out, err := exec.Command("tasklist", "/FI", "IMAGENAME eq "+gdExe, "/NH").Output()
	return err == nil && strings.Contains(strings.ToLower(string(out)), strings.ToLower(gdExe))
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	if _, err := io.Copy(out, in); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}

func exeDir() string {
	exe, err := os.Executable()
	if err != nil {
		return "."
	}
	return filepath.Dir(exe)
}

func ask(prompt string) string {
	fmt.Print(prompt)
	line, _ := stdin.ReadString('\n')
	return strings.TrimSpace(line)
}

func exitWait(code int) {
	fmt.Print("\nPress Enter to close...")
	stdin.ReadString('\n')
	os.Exit(code)
}
