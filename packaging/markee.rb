class Markee < Formula
  desc "Desktop 72x16 24-bit RGB LED matrix display simulator with HTTP API"
  homepage "https://github.com/stevenspencer/markee"
  url "https://github.com/stevenspencer/markee/archive/refs/tags/v1.0.0.tar.gz"
  sha256 "REPLACE_WITH_RELEASE_TARBALL_SHA256"
  license "MIT"
  head "https://github.com/stevenspencer/markee.git", branch: "master"

  depends_on "pkg-config" => :build
  depends_on "raylib"

  def install
    system "make", "release"
    bin.install "markee"
    man1.install "docs/markee.1"
    doc.install "README.md", "docs/API.md"
    bash_completion.install "completions/markee.bash" => "markee"
    zsh_completion.install "completions/_markee"
    fish_completion.install "completions/markee.fish"
    (pkgshare/"examples").install Dir["examples/*"]
  end

  service do
    run [opt_bin/"markee", "--always-on-top", "--borderless", "--corner", "top-right", "--save-position"]
    keep_alive false
    log_path var/"log/markee.log"
    error_log_path var/"log/markee.log"
  end

  test do
    assert_match "Usage:", shell_output("#{bin}/markee --help")
  end
end
