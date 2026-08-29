# Upload this preservation set to a private GitHub repository

The prepared folder is:

```text
F:\DEV\QuakeIIPS3\github-v221
```

The `release-assets` directory is intentionally ignored by Git. Its ISO and PKG
are larger than Git's normal file limit and contain commercial game data. They
belong on a **private GitHub Release**, not in repository history.

## 1. Install and authenticate GitHub CLI

GitHub CLI is not currently installed on this workstation. In PowerShell:

```powershell
winget install --id GitHub.cli
```

Open a new PowerShell window, then authenticate:

```powershell
gh auth login
```

If the same PowerShell window still says that `gh` is not recognized, either
close and reopen PowerShell or refresh that window's PATH before continuing:

```powershell
$env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
  [Environment]::GetEnvironmentVariable('Path', 'User')
gh --version
gh auth login
```

You can also bypass PATH lookup and invoke the installed executable directly:

```powershell
& 'C:\Program Files\GitHub CLI\gh.exe' auth login
```

Choose GitHub.com, HTTPS, and browser authentication when prompted.

## 2. Create the local commit

The folder has already been initialized as a `main` Git repository and all
allowed source/documentation files are staged. No Git author identity is
configured on this workstation, so set your own identity if Git requests it:

```powershell
Set-Location F:\DEV\QuakeIIPS3\github-v221
git config user.name "YOUR NAME"
git config user.email "YOUR GITHUB EMAIL"
git status --short
git commit -m "Preserve Quake II PS3 v2.21"
```

Before committing, confirm that `release-assets`, `*.pak`, `*.iso`, and `*.pkg`
do **not** appear in `git status`.

## 3. Create and push the private repository

Replace the repository name if desired:

```powershell
gh repo create quake-ii-ps3-v221 --private --source . --remote origin --push
gh repo view --json nameWithOwner,visibility,url
```

Do not continue unless the returned visibility is `PRIVATE`.

## 4. Upload the binary containers as a draft private release

```powershell
git tag -a v2.21 -m "Quake II PS3 v2.21"
git push origin v2.21

gh release create v2.21 `
  ".\release-assets\Quake-II-PS3-v2.21.iso#Standalone PS3 ISO" `
  ".\release-assets\Quake-II-PS3-v2.21-full.gnpdrm.pkg#Full install PKG" `
  ".\release-assets\SHA256SUMS.txt#SHA-256 checksums" `
  --draft `
  --title "Quake II PS3 v2.21" `
  --notes-file .\docs\RELEASE_NOTES_v2.21.md
```

Open the draft for a final check:

```powershell
gh release view v2.21 --web
```

Because the repository is private, only authorized repository users can see a
published release. Keeping it as a draft adds another safety check before any
collaborator can download it. GitHub currently permits individual release
assets under 2 GiB; both prepared files are about 201 MB.

## Browser-only alternative

1. On GitHub, create a new repository and explicitly choose **Private**.
2. Do not initialize it with a README or license.
3. Create the local commit using the commands above.
4. Follow the empty repository's displayed commands to add `origin` and push
   `main`.
5. Open **Releases**, create a new draft release for tag `v2.21`, and drag the
   three files from `release-assets` into the asset area.

Useful references:

- https://docs.github.com/en/migrations/importing-source-code/using-the-command-line-to-import-source-code/adding-locally-hosted-code-to-github
- https://cli.github.com/manual/gh_repo_create
- https://cli.github.com/manual/gh_release_create
- https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases
