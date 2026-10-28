package main

import (
	"bufio"
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"strings"
	"time"

	"github.com/phabioo/uzume/server/internal/config"
	"github.com/phabioo/uzume/server/internal/hub"
	"github.com/phabioo/uzume/server/internal/updates"
)

// dockerImage is the official image; a container is updated by pulling the new tag (wiki/Server-in-Docker.md).
const dockerImage = "ghcr.io/phabioo/uzume-server"

// runUpdateNow is `uzume-server update` without a sub-command: it looks for a newer version on the server's update
// channel and installs it the way "Install update" in the app does (the update is staged and the request is handed to
// the root helper on Linux and to the update service on Windows). In a container it names the image tag to pull.
func runUpdateNow(args []string, in io.Reader, out io.Writer) error {
	fs := flag.NewFlagSet("update", flag.ContinueOnError)
	cfg := config.Register(fs, os.Getenv)
	checkOnly := fs.Bool("check", false, "only check, do not install")
	channel := fs.String("channel", "", "with -check: look on this channel (stable or beta) instead of the server setting")
	yes := fs.Bool("yes", false, "do not ask for confirmation")
	confirmBreaking := fs.Bool("confirm-breaking", false, "install an update that apps seen in the last 30 days are too old for")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if fs.NArg() > 0 {
		return errors.New(updateUsage)
	}
	if *channel != "" && !*checkOnly {
		return errors.New("-channel works with -check only; the channel to install from is set in Settings › Server › Updates")
	}
	if err := cfg.Validate(); err != nil {
		return err
	}
	res, err := loadUpdateCheck(cfg, *channel, false)
	if err != nil {
		return err
	}
	fmt.Fprintf(out, "Uzume server %s, channel %s.\n", res.Current, res.Channel)
	if res.UpToDate || res.Available == nil {
		fmt.Fprintln(out, "This is the newest version on the channel.")
		return nil
	}
	a := res.Available
	fmt.Fprintf(out, "Newer version: %s (%s, published %s).\n", a.Version, a.Channel, a.PublishedAt.Local().Format("2 Jan 2006"))
	if a.NotesURL != "" {
		fmt.Fprintf(out, "Release notes: %s\n", a.NotesURL)
	}
	if updates.InContainer() {
		fmt.Fprintf(out, "This server runs in a container and cannot update itself. Pull the new image and recreate the container:\n  docker pull %s:%s\n  (docker compose: set the tag, then 'docker compose pull && docker compose up -d')\n", dockerImage, a.Version)
		return nil
	}
	if *checkOnly {
		fmt.Fprintln(out, "Install it with: uzume-server update")
		return nil
	}

	ctx, cancel := context.WithTimeout(context.Background(), 45*time.Minute)
	defer cancel()
	svc, closeFn, err := openService(ctx, cfg, false)
	if err != nil {
		return err
	}
	defer closeFn()
	if !*yes {
		fmt.Fprintf(out, "Install %s now? The server restarts (about 1 minute) and connected apps disconnect for that time. [y/N] ", a.Version)
		line, _ := bufio.NewReader(in).ReadString('\n')
		if ans := strings.ToLower(strings.TrimSpace(line)); ans != "y" && ans != "yes" {
			return errors.New("canceled")
		}
	}
	st, err := svc.InstallUpdate(ctx, *confirmBreaking)
	switch {
	case err == nil:
	case errors.Is(err, hub.ErrNotPackaged):
		if us, uerr := svc.UpdateStatus(ctx); uerr == nil && us.ManualCommand != "" {
			fmt.Fprintf(out, "This install cannot update itself. By hand:\n  %s\n", us.ManualCommand)
		}
		return err
	case errors.Is(err, hub.ErrUpdateBreaking):
		return errors.New("apps seen in the last 30 days are too old for this version and would lose access until they are updated; run again with -confirm-breaking to install it anyway")
	default:
		return err
	}
	fmt.Fprintf(out, "Staged %s; the installer was requested. The server restarts in a moment.\n", st.Version)
	return nil
}
