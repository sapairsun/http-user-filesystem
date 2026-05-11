// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package main

import (
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"
)

type server struct {
	rootDir string
}

type errorResponse struct {
	Status  string `json:"status"`
	Errno   int    `json:"errno"`
	Message string `json:"message"`
}

type dirEntry struct {
	Name string `json:"name"`
	Type string `json:"type"`
}

type metaObject struct {
	Path  string `json:"path"`
	Type  string `json:"type"`
	Mode  uint32 `json:"mode"`
	Size  int64  `json:"size"`
	UID   uint32 `json:"uid"`
	GID   uint32 `json:"gid"`
	Nlink uint64 `json:"nlink"`
	Atime int64  `json:"atime"`
	Mtime int64  `json:"mtime"`
	Ctime int64  `json:"ctime"`
}

const (
	utimeNow  = int64((1 << 30) - 1)
	utimeOmit = int64((1 << 30) - 2)
)

func main() {
	port := flag.Int("port", 0, "listen port")
	rootDir := flag.String("root-dir", "", "backend root directory")
	host := flag.String("host", "127.0.0.1", "listen host")
	flag.Parse()

	if *port <= 0 || *rootDir == "" {
		fmt.Fprintln(os.Stderr, "Usage: go-service_provider --port <port> --root-dir <backend_root_dir> [--host <host>]")
		os.Exit(1)
	}

	absRoot, err := filepath.Abs(*rootDir)
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed to resolve backend root: %v\n", err)
		os.Exit(1)
	}

	srv := &server{rootDir: absRoot}
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/meta", srv.wrapGET(srv.handleMeta))
	mux.HandleFunc("/v1/list", srv.wrapGET(srv.handleList))
	mux.HandleFunc("/v1/read", srv.wrapGET(srv.handleRead))
	mux.HandleFunc("/v1/write", srv.wrapPOST(srv.handleWrite))
	mux.HandleFunc("/v1/create-file", srv.wrapPOST(srv.handleCreateFile))
	mux.HandleFunc("/v1/create-dir", srv.wrapPOST(srv.handleCreateDir))
	mux.HandleFunc("/v1/remove-file", srv.wrapPOST(srv.handleRemoveFile))
	mux.HandleFunc("/v1/remove-dir", srv.wrapPOST(srv.handleRemoveDir))
	mux.HandleFunc("/v1/rename", srv.wrapPOST(srv.handleRename))
	mux.HandleFunc("/v1/truncate", srv.wrapPOST(srv.handleTruncate))
	mux.HandleFunc("/v1/utimens", srv.wrapPOST(srv.handleUtimens))

	addr := fmt.Sprintf("%s:%d", *host, *port)
	if err := http.ListenAndServe(addr, mux); err != nil {
		fmt.Fprintf(os.Stderr, "listen failed: %v\n", err)
		os.Exit(1)
	}
}

func (s *server) wrapGET(handler func(http.ResponseWriter, *http.Request) error) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			writeError(w, syscall.ENOSYS, "unsupported method")
			return
		}
		if err := handler(w, r); err != nil {
			writeMappedError(w, err)
		}
	}
}

func (s *server) wrapPOST(handler func(http.ResponseWriter, *http.Request) error) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			writeError(w, syscall.ENOSYS, "unsupported method")
			return
		}
		if err := handler(w, r); err != nil {
			writeMappedError(w, err)
		}
	}
}

func (s *server) handleMeta(w http.ResponseWriter, r *http.Request) error {
	path := r.URL.Query().Get("path")
	localPath, err := s.resolvePath(path)
	if err != nil {
		return err
	}

	info, err := os.Lstat(localPath)
	if err != nil {
		return err
	}

	return writeJSON(w, map[string]any{
		"status": "ok",
		"meta":   metaFromInfo(path, info),
	})
}

func (s *server) handleList(w http.ResponseWriter, r *http.Request) error {
	path := r.URL.Query().Get("path")
	localPath, err := s.resolvePath(path)
	if err != nil {
		return err
	}

	names, err := readSortedDirNames(localPath)
	if err != nil {
		return err
	}

	entries := []dirEntry{
		{Name: ".", Type: "dir"},
		{Name: "..", Type: "dir"},
	}
	for _, name := range names {
		entryPath := filepath.Join(localPath, name)
		entryInfo, statErr := os.Lstat(entryPath)
		if statErr != nil {
			return statErr
		}
		entries = append(entries, dirEntry{
			Name: name,
			Type: typeFromMode(entryInfo.Mode()),
		})
	}

	return writeJSON(w, map[string]any{
		"status":  "ok",
		"entries": entries,
	})
}

func (s *server) handleRead(w http.ResponseWriter, r *http.Request) error {
	path := r.URL.Query().Get("path")
	offset, err := parseNonNegativeInt64(r.URL.Query().Get("offset"), "offset")
	if err != nil {
		return err
	}
	size, err := parseNonNegativeInt64(r.URL.Query().Get("size"), "size")
	if err != nil {
		return err
	}
	localPath, err := s.resolvePath(path)
	if err != nil {
		return err
	}

	file, err := os.Open(localPath)
	if err != nil {
		return err
	}
	defer file.Close()

	if _, err = file.Seek(offset, io.SeekStart); err != nil {
		return err
	}

	buffer := make([]byte, size)
	bytesRead, err := file.Read(buffer)
	if err != nil && !errors.Is(err, io.EOF) {
		return err
	}

	return writeJSON(w, map[string]any{
		"status":     "ok",
		"data_hex":   hex.EncodeToString(buffer[:bytesRead]),
		"bytes_read": bytesRead,
	})
}

func (s *server) handleWrite(w http.ResponseWriter, r *http.Request) error {
	var req struct {
		Path    string `json:"path"`
		Offset  int64  `json:"offset"`
		DataHex string `json:"data_hex"`
	}

	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		return invalidArgument("invalid write body")
	}
	if req.Offset < 0 {
		return invalidArgument("offset must be non-negative")
	}

	localPath, err := s.resolvePath(req.Path)
	if err != nil {
		return err
	}
	data, err := hex.DecodeString(req.DataHex)
	if err != nil {
		return invalidArgument("invalid hex body")
	}

	file, err := os.OpenFile(localPath, os.O_WRONLY, 0)
	if err != nil {
		return err
	}
	defer file.Close()

	if _, err = file.Seek(req.Offset, io.SeekStart); err != nil {
		return err
	}
	bytesWritten, err := file.Write(data)
	if err != nil {
		return err
	}

	return writeJSON(w, map[string]any{
		"status":        "ok",
		"bytes_written": bytesWritten,
	})
}

func (s *server) handleCreateFile(w http.ResponseWriter, r *http.Request) error {
	var req struct {
		Path string `json:"path"`
		Mode uint32 `json:"mode"`
	}

	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		return invalidArgument("invalid create-file body")
	}

	localPath, err := s.resolvePath(req.Path)
	if err != nil {
		return err
	}

	file, err := os.OpenFile(localPath, os.O_CREATE|os.O_EXCL|os.O_WRONLY, os.FileMode(req.Mode))
	if err != nil {
		return err
	}
	if err = file.Close(); err != nil {
		return err
	}
	return writeJSON(w, map[string]any{"status": "ok"})
}

func (s *server) handleCreateDir(w http.ResponseWriter, r *http.Request) error {
	var req struct {
		Path string `json:"path"`
		Mode uint32 `json:"mode"`
	}

	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		return invalidArgument("invalid create-dir body")
	}

	localPath, err := s.resolvePath(req.Path)
	if err != nil {
		return err
	}

	if err = os.Mkdir(localPath, os.FileMode(req.Mode)); err != nil {
		return err
	}
	return writeJSON(w, map[string]any{"status": "ok"})
}

func (s *server) handleRemoveFile(w http.ResponseWriter, r *http.Request) error {
	var req struct {
		Path string `json:"path"`
	}

	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		return invalidArgument("invalid remove-file body")
	}

	localPath, err := s.resolvePath(req.Path)
	if err != nil {
		return err
	}
	if err = os.Remove(localPath); err != nil {
		return err
	}
	return writeJSON(w, map[string]any{"status": "ok"})
}

func (s *server) handleRemoveDir(w http.ResponseWriter, r *http.Request) error {
	var req struct {
		Path string `json:"path"`
	}

	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		return invalidArgument("invalid remove-dir body")
	}

	localPath, err := s.resolvePath(req.Path)
	if err != nil {
		return err
	}
	if err = os.Remove(localPath); err != nil {
		return err
	}
	return writeJSON(w, map[string]any{"status": "ok"})
}

func (s *server) handleRename(w http.ResponseWriter, r *http.Request) error {
	var req struct {
		OldPath string `json:"old_path"`
		NewPath string `json:"new_path"`
	}

	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		return invalidArgument("invalid rename body")
	}

	oldPath, err := s.resolvePath(req.OldPath)
	if err != nil {
		return err
	}
	newPath, err := s.resolvePath(req.NewPath)
	if err != nil {
		return err
	}
	if err = os.Rename(oldPath, newPath); err != nil {
		return err
	}
	return writeJSON(w, map[string]any{"status": "ok"})
}

func (s *server) handleTruncate(w http.ResponseWriter, r *http.Request) error {
	var req struct {
		Path string `json:"path"`
		Size int64  `json:"size"`
	}

	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		return invalidArgument("invalid truncate body")
	}
	if req.Size < 0 {
		return invalidArgument("size must be non-negative")
	}

	localPath, err := s.resolvePath(req.Path)
	if err != nil {
		return err
	}
	if err = os.Truncate(localPath, req.Size); err != nil {
		return err
	}
	return writeJSON(w, map[string]any{"status": "ok"})
}

func (s *server) handleUtimens(w http.ResponseWriter, r *http.Request) error {
	var req struct {
		Path      string `json:"path"`
		AtimeSec  int64  `json:"atime_sec"`
		AtimeNsec int64  `json:"atime_nsec"`
		MtimeSec  int64  `json:"mtime_sec"`
		MtimeNsec int64  `json:"mtime_nsec"`
	}

	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		return invalidArgument("invalid utimens body")
	}

	localPath, err := s.resolvePath(req.Path)
	if err != nil {
		return err
	}

	info, err := os.Lstat(localPath)
	if err != nil {
		return err
	}

	atime, err := normalizeUtimensTime(info, req.AtimeSec, req.AtimeNsec, true)
	if err != nil {
		return err
	}
	mtime, err := normalizeUtimensTime(info, req.MtimeSec, req.MtimeNsec, false)
	if err != nil {
		return err
	}

	if err = os.Chtimes(localPath, atime, mtime); err != nil {
		return err
	}
	return writeJSON(w, map[string]any{"status": "ok"})
}

func (s *server) resolvePath(path string) (string, error) {
	if !strings.HasPrefix(path, "/") {
		return "", invalidArgument("path must start with '/'")
	}
	if path == "/" {
		return s.rootDir, nil
	}

	candidate := filepath.Join(s.rootDir, filepath.Clean(path))
	candidate, err := filepath.Abs(candidate)
	if err != nil {
		return "", err
	}
	rootWithSep := s.rootDir + string(os.PathSeparator)
	if candidate != s.rootDir && !strings.HasPrefix(candidate, rootWithSep) {
		return "", invalidArgument("path escapes backend root")
	}
	return candidate, nil
}

func metaFromInfo(path string, info os.FileInfo) metaObject {
	stat, ok := info.Sys().(*syscall.Stat_t)
	if !ok {
		return metaObject{
			Path:  path,
			Type:  typeFromMode(info.Mode()),
			Mode:  uint32(info.Mode()),
			Size:  info.Size(),
			UID:   0,
			GID:   0,
			Nlink: 1,
			Atime: info.ModTime().Unix(),
			Mtime: info.ModTime().Unix(),
			Ctime: info.ModTime().Unix(),
		}
	}

	atime, mtime, ctime := extractTimes(stat, info.ModTime())
	return metaObject{
		Path:  path,
		Type:  typeFromMode(info.Mode()),
		Mode:  uint32(stat.Mode),
		Size:  info.Size(),
		UID:   stat.Uid,
		GID:   stat.Gid,
		Nlink: uint64(stat.Nlink),
		Atime: atime,
		Mtime: mtime,
		Ctime: ctime,
	}
}

func typeFromMode(mode os.FileMode) string {
	if mode.IsDir() {
		return "dir"
	}
	return "file"
}

func readSortedDirNames(path string) ([]string, error) {
	entries, err := os.ReadDir(path)
	if err != nil {
		return nil, err
	}

	names := make([]string, 0, len(entries))
	for _, entry := range entries {
		names = append(names, entry.Name())
	}
	sort.Strings(names)
	return names, nil
}

func parseNonNegativeInt64(raw string, name string) (int64, error) {
	value, err := strconv.ParseInt(raw, 10, 64)
	if err != nil || value < 0 {
		return 0, invalidArgument(name + " must be a non-negative integer")
	}
	return value, nil
}

func writeJSON(w http.ResponseWriter, payload any) error {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	return json.NewEncoder(w).Encode(payload)
}

func writeError(w http.ResponseWriter, errnoValue syscall.Errno, message string) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	_ = json.NewEncoder(w).Encode(errorResponse{
		Status:  "error",
		Errno:   int(errnoValue),
		Message: message,
	})
}

func writeMappedError(w http.ResponseWriter, err error) {
	var pathErr *os.PathError
	var syscallErr syscall.Errno
	switch {
	case errors.As(err, &pathErr):
		if errors.As(pathErr.Err, &syscallErr) {
			writeError(w, syscallErr, pathErr.Err.Error())
			return
		}
	case errors.As(err, &syscallErr):
		writeError(w, syscallErr, err.Error())
		return
	}

	var invalid *invalidArgError
	if errors.As(err, &invalid) {
		writeError(w, syscall.EINVAL, invalid.Error())
		return
	}

	writeError(w, syscall.EIO, err.Error())
}

type invalidArgError struct {
	message string
}

func (e *invalidArgError) Error() string {
	return e.message
}

func invalidArgument(message string) error {
	return &invalidArgError{message: message}
}

func normalizeUtimensTime(info os.FileInfo, sec int64, nsec int64, isAtime bool) (time.Time, error) {
	switch nsec {
	case utimeNow:
		return time.Now(), nil
	case utimeOmit:
		if isAtime {
			if ns, ok := statTimestampNS(info, "Atim", "Atimespec"); ok {
				return time.Unix(0, ns), nil
			}
		} else {
			if ns, ok := statTimestampNS(info, "Mtim", "Mtimespec"); ok {
				return time.Unix(0, ns), nil
			}
		}
		return info.ModTime(), nil
	default:
		if nsec < 0 || nsec >= 1_000_000_000 {
			return time.Time{}, invalidArgument("nanoseconds must be in [0, 1000000000)")
		}
		return time.Unix(sec, nsec), nil
	}
}

func statTimestampNS(info os.FileInfo, names ...string) (int64, bool) {
	stat, ok := info.Sys().(*syscall.Stat_t)
	if !ok {
		return 0, false
	}
	value := reflectStatField(stat, names...)
	return timespecNanoseconds(value)
}

func extractTimes(stat *syscall.Stat_t, fallback time.Time) (int64, int64, int64) {
	fallbackUnix := fallback.Unix()
	atime := fallbackUnix
	mtime := fallbackUnix
	ctime := fallbackUnix

	if stat == nil {
		return atime, mtime, ctime
	}

	value := reflectStatField(stat, "Atim", "Atimespec")
	if sec, ok := timespecSeconds(value); ok {
		atime = sec
	}
	value = reflectStatField(stat, "Mtim", "Mtimespec")
	if sec, ok := timespecSeconds(value); ok {
		mtime = sec
	}
	value = reflectStatField(stat, "Ctim", "Ctimespec")
	if sec, ok := timespecSeconds(value); ok {
		ctime = sec
	}

	return atime, mtime, ctime
}

func reflectStatField(stat *syscall.Stat_t, names ...string) any {
	rv := reflect.ValueOf(stat).Elem()
	for _, name := range names {
		field := rv.FieldByName(name)
		if field.IsValid() {
			return field.Interface()
		}
	}
	return nil
}

func timespecSeconds(value any) (int64, bool) {
	if value == nil {
		return 0, false
	}

	rv := reflect.ValueOf(value)
	if rv.Kind() != reflect.Struct {
		return 0, false
	}

	for _, fieldName := range []string{"Sec", "Tv_sec"} {
		field := rv.FieldByName(fieldName)
		if field.IsValid() {
			switch field.Kind() {
			case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
				return field.Int(), true
			}
		}
	}

	return 0, false
}

func timespecNanoseconds(value any) (int64, bool) {
	if value == nil {
		return 0, false
	}

	rv := reflect.ValueOf(value)
	if rv.Kind() != reflect.Struct {
		return 0, false
	}

	var sec int64
	var nsec int64
	var okSec bool
	var okNsec bool

	for _, fieldName := range []string{"Sec", "Tv_sec"} {
		field := rv.FieldByName(fieldName)
		if field.IsValid() {
			switch field.Kind() {
			case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
				sec = field.Int()
				okSec = true
			}
		}
	}
	for _, fieldName := range []string{"Nsec", "Tv_nsec"} {
		field := rv.FieldByName(fieldName)
		if field.IsValid() {
			switch field.Kind() {
			case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
				nsec = field.Int()
				okNsec = true
			}
		}
	}

	if !okSec || !okNsec {
		return 0, false
	}
	return sec*1_000_000_000 + nsec, true
}
