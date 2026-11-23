using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Text;
using System.Windows.Forms;

namespace VistaWimExtractor
{
    static class Program
    {
        [STAThread]
        static void Main(string[] args)
        {
            if (args != null && args.Length > 0)
            {
                // CLI mode
                RunCommandLine(args);
            }
            else
            {
                // GUI mode
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.Run(new MainForm());
            }
        }

        static void RunCommandLine(string[] args)
        {
            Console.WriteLine("=== Windows Vista File Extractor for explorer7 / explorerVista ===");
            string inputPath = args[0];
            string outputDir = (args.Length > 1) ? args[1] : Directory.GetCurrentDirectory();
            bool autoPatch = true;

            for (int i = 2; i < args.Length; i++)
            {
                if (args[i].Equals("/nopatch", StringComparison.OrdinalIgnoreCase))
                    autoPatch = false;
            }

            ExtractorEngine engine = new ExtractorEngine();
            engine.OnLog += (msg) => Console.WriteLine(msg);

            try
            {
                bool ok = engine.Extract(inputPath, outputDir, autoPatch);
                Console.WriteLine(ok ? "[SUCCESS] All files extracted successfully." : "[FAILED] Extraction failed.");
            }
            catch (Exception ex)
            {
                Console.WriteLine("[ERROR] " + ex.Message);
            }
        }
    }

    public class MainForm : Form
    {
        private TextBox txtInput;
        private TextBox txtOutput;
        private CheckBox chkAutoPatch;
        private CheckBox chkIncludeTheme;
        private Button btnBrowseInput;
        private Button btnDetectDrives;
        private Button btnBrowseOutput;
        private Button btnExtract;
        private ListBox lstLog;
        private ProgressBar progressBar;
        private Label lblStatus;
        private BackgroundWorker worker;

        public MainForm()
        {
            InitializeComponent();
            CheckDefaultDrives();
        }

        private void InitializeComponent()
        {
            this.Text = "Windows Vista File Extractor for explorerVista / explorer7";
            this.Size = new Size(680, 540);
            this.StartPosition = FormStartPosition.CenterScreen;
            this.Font = new Font("Segoe UI", 9F);
            this.FormBorderStyle = FormBorderStyle.FixedDialog;
            this.MaximizeBox = false;

            Label lblInput = new Label { Text = "Vista Source (ISO, WIM, or Mounted Drive):", Location = new Point(20, 15), Size = new Size(350, 20) };
            txtInput = new TextBox { Location = new Point(20, 38), Size = new Size(460, 25) };
            btnBrowseInput = new Button { Text = "Browse...", Location = new Point(490, 37), Size = new Size(75, 27) };
            btnDetectDrives = new Button { Text = "Detect ISO", Location = new Point(570, 37), Size = new Size(80, 27) };

            Label lblOutput = new Label { Text = "Destination Folder:", Location = new Point(20, 75), Size = new Size(200, 20) };
            txtOutput = new TextBox { Location = new Point(20, 98), Size = new Size(545, 25), Text = AppDomain.CurrentDomain.BaseDirectory };
            btnBrowseOutput = new Button { Text = "Browse...", Location = new Point(575, 97), Size = new Size(75, 27) };

            chkAutoPatch = new CheckBox
            {
                Text = "Automatically patch explorer.exe PE imports (redirect to wrp64.dll)",
                Location = new Point(20, 135),
                Size = new Size(500, 24),
                Checked = true
            };

            chkIncludeTheme = new CheckBox
            {
                Text = "Extract Vista Aero theme (aero.msstyles -> theme\\vista.msstyles)",
                Location = new Point(20, 162),
                Size = new Size(500, 24),
                Checked = true
            };

            btnExtract = new Button
            {
                Text = "Start Search & Extraction",
                Location = new Point(20, 195),
                Size = new Size(630, 38),
                Font = new Font("Segoe UI", 10F, FontStyle.Bold),
                BackColor = Color.FromArgb(0, 120, 215),
                ForeColor = Color.White
            };

            progressBar = new ProgressBar { Location = new Point(20, 242), Size = new Size(630, 15), Style = ProgressBarStyle.Marquee, MarqueeAnimationSpeed = 0 };
            lblStatus = new Label { Text = "Ready. Select a Windows Vista ISO or install.wim to begin.", Location = new Point(20, 262), Size = new Size(630, 20) };

            lstLog = new ListBox { Location = new Point(20, 285), Size = new Size(630, 200), HorizontalScrollbar = true };

            btnBrowseInput.Click += BtnBrowseInput_Click;
            btnDetectDrives.Click += (s, e) => CheckDefaultDrives();
            btnBrowseOutput.Click += BtnBrowseOutput_Click;
            btnExtract.Click += BtnExtract_Click;

            this.Controls.Add(lblInput);
            this.Controls.Add(txtInput);
            this.Controls.Add(btnBrowseInput);
            this.Controls.Add(btnDetectDrives);
            this.Controls.Add(lblOutput);
            this.Controls.Add(txtOutput);
            this.Controls.Add(btnBrowseOutput);
            this.Controls.Add(chkAutoPatch);
            this.Controls.Add(chkIncludeTheme);
            this.Controls.Add(btnExtract);
            this.Controls.Add(progressBar);
            this.Controls.Add(lblStatus);
            this.Controls.Add(lstLog);

            worker = new BackgroundWorker();
            worker.WorkerReportsProgress = true;
            worker.DoWork += Worker_DoWork;
            worker.RunWorkerCompleted += Worker_RunWorkerCompleted;
        }

        private void CheckDefaultDrives()
        {
            foreach (var drive in DriveInfo.GetDrives())
            {
                try
                {
                    if (drive.IsReady)
                    {
                        string wim = Path.Combine(drive.RootDirectory.FullName, @"sources\install.wim");
                        if (File.Exists(wim))
                        {
                            txtInput.Text = wim;
                            Log("Detected install.wim on drive: " + drive.Name);
                            return;
                        }
                    }
                }
                catch { }
            }

            // Check downloads or desktop for vista iso
            string userProfile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            string[] searchDirs = { Path.Combine(userProfile, "Downloads"), Path.Combine(userProfile, "Desktop"), @"C:\" };
            foreach (var dir in searchDirs)
            {
                if (Directory.Exists(dir))
                {
                    try
                    {
                        var isos = Directory.GetFiles(dir, "*vista*.iso", SearchOption.TopDirectoryOnly);
                        if (isos.Length > 0)
                        {
                            txtInput.Text = isos[0];
                            Log("Found candidate Vista ISO: " + isos[0]);
                            return;
                        }
                    }
                    catch { }
                }
            }
        }

        private void BtnBrowseInput_Click(object sender, EventArgs e)
        {
            using (OpenFileDialog ofd = new OpenFileDialog())
            {
                ofd.Title = "Select Windows Vista ISO or install.wim";
                ofd.Filter = "Vista Installation Source (*.iso;*.wim)|*.iso;*.wim|All Files (*.*)|*.*";
                if (ofd.ShowDialog() == DialogResult.OK)
                {
                    txtInput.Text = ofd.FileName;
                }
            }
        }

        private void BtnBrowseOutput_Click(object sender, EventArgs e)
        {
            using (FolderBrowserDialog fbd = new FolderBrowserDialog())
            {
                fbd.Description = "Select Destination Folder for Vista Files";
                fbd.SelectedPath = txtOutput.Text;
                if (fbd.ShowDialog() == DialogResult.OK)
                {
                    txtOutput.Text = fbd.SelectedPath;
                }
            }
        }

        private void BtnExtract_Click(object sender, EventArgs e)
        {
            if (string.IsNullOrEmpty(txtInput.Text) || (!File.Exists(txtInput.Text) && !Directory.Exists(txtInput.Text)))
            {
                MessageBox.Show("Please select a valid Windows Vista ISO or install.wim file.", "Input Required", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            if (string.IsNullOrEmpty(txtOutput.Text))
            {
                MessageBox.Show("Please select an output directory.", "Output Required", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            btnExtract.Enabled = false;
            progressBar.MarqueeAnimationSpeed = 30;
            lblStatus.Text = "Searching and extracting files from source...";
            lstLog.Items.Clear();

            worker.RunWorkerAsync(new ExtractionParams
            {
                Source = txtInput.Text,
                Destination = txtOutput.Text,
                AutoPatch = chkAutoPatch.Checked,
                IncludeTheme = chkIncludeTheme.Checked
            });
        }

        private void Worker_DoWork(object sender, DoWorkEventArgs e)
        {
            ExtractionParams p = (ExtractionParams)e.Argument;
            ExtractorEngine engine = new ExtractorEngine();
            engine.OnLog += (msg) =>
            {
                this.Invoke(new Action(() => Log(msg)));
            };

            bool ok = engine.Extract(p.Source, p.Destination, p.AutoPatch, p.IncludeTheme);
            e.Result = ok;
        }

        private void Worker_RunWorkerCompleted(object sender, RunWorkerCompletedEventArgs e)
        {
            btnExtract.Enabled = true;
            progressBar.MarqueeAnimationSpeed = 0;

            if (e.Error != null)
            {
                lblStatus.Text = "Error during extraction: " + e.Error.Message;
                MessageBox.Show("Extraction failed:\n" + e.Error.Message, "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            else if ((bool)e.Result)
            {
                lblStatus.Text = "Completed successfully!";
                MessageBox.Show("Windows Vista shell files successfully extracted and organized!\n\nDestination: " + txtOutput.Text, "Extraction Complete", MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            else
            {
                lblStatus.Text = "Extraction finished with warnings or missing files.";
            }
        }

        private void Log(string msg)
        {
            lstLog.Items.Add(msg);
            lstLog.SelectedIndex = lstLog.Items.Count - 1;
        }

        private class ExtractionParams
        {
            public string Source;
            public string Destination;
            public bool AutoPatch;
            public bool IncludeTheme;
        }
    }

    public class ExtractorEngine
    {
        public event Action<string> OnLog;
        private void Log(string msg) { if (OnLog != null) OnLog(msg); }

        public bool Extract(string sourcePath, string outputDir, bool autoPatch, bool includeTheme = true)
        {
            string sevenZip = Find7Zip();
            if (string.IsNullOrEmpty(sevenZip))
            {
                Log("[ERROR] 7-Zip (7z.exe) not found on system. Please install 7-Zip to extract directly from WIM/ISO.");
                return false;
            }

            Log("[INFO] Using 7-Zip binary: " + sevenZip);
            Directory.CreateDirectory(outputDir);

            string wimPath = sourcePath;
            string tempDir = null;

            try
            {
                // Step 1: If source is ISO, extract install.wim to temp dir
                if (sourcePath.EndsWith(".iso", StringComparison.OrdinalIgnoreCase))
                {
                    Log("[INFO] Source is an ISO. Locating install.wim inside ISO...");
                    tempDir = Path.Combine(Path.GetTempPath(), "VistaExtract_" + Guid.NewGuid().ToString("N"));
                    Directory.CreateDirectory(tempDir);

                    string runArgs = string.Format("e \"{0}\" \"sources\\install.wim\" -o\"{1}\" -y", sourcePath, tempDir);
                    if (!RunProcess(sevenZip, runArgs))
                    {
                        Log("[ERROR] Failed to extract sources\\install.wim from ISO.");
                        return false;
                    }

                    wimPath = Path.Combine(tempDir, "install.wim");
                    if (!File.Exists(wimPath))
                    {
                        Log("[ERROR] install.wim not found inside ISO.");
                        return false;
                    }
                    Log("[OK] Extracted temporary install.wim (" + (new FileInfo(wimPath).Length / 1024 / 1024) + " MB)");
                }

                // Step 2: List targeted entries in WIM to detect image prefix (single vs multi-image) and language
                Log("[INFO] Analyzing contents of " + Path.GetFileName(wimPath) + "...");
                string listOutput = RunProcessWithOutput(sevenZip, string.Format("l \"{0}\" -r \"explorer.exe\" \"explorer.exe.mui\"", wimPath));

                string imagePrefix = null;
                string detectedLang = null;

                string[] lines = listOutput.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
                foreach (var rawLine in lines)
                {
                    string line = rawLine.Trim();
                    // 7z listing lines end with the path; skip headers/separators
                    int lastSpace = line.LastIndexOf(' ');
                    if (lastSpace < 0) continue;
                    string fullPath = line.Substring(lastSpace + 1).Trim();

                    if (fullPath.IndexOf("\\winsxs\\", StringComparison.OrdinalIgnoreCase) >= 0 ||
                        fullPath.IndexOf("\\SysWOW64\\", StringComparison.OrdinalIgnoreCase) >= 0)
                    {
                        continue;
                    }

                    // Match Windows\explorer.exe or <index>\Windows\explorer.exe
                    if (fullPath.Equals("Windows\\explorer.exe", StringComparison.OrdinalIgnoreCase))
                    {
                        imagePrefix = "";
                    }
                    else if (fullPath.EndsWith("\\Windows\\explorer.exe", StringComparison.OrdinalIgnoreCase))
                    {
                        int winIdx = fullPath.IndexOf("\\Windows\\explorer.exe", StringComparison.OrdinalIgnoreCase);
                        string candidatePrefix = fullPath.Substring(0, winIdx + 1);
                        if (imagePrefix == null) imagePrefix = candidatePrefix;
                    }

                    // Match [prefix]Windows\<lang>\explorer.exe.mui
                    if (fullPath.EndsWith("\\explorer.exe.mui", StringComparison.OrdinalIgnoreCase))
                    {
                        var segs = fullPath.Split('\\');
                        for (int i = 0; i + 2 < segs.Length; i++)
                        {
                            if (segs[i].Equals("Windows", StringComparison.OrdinalIgnoreCase) &&
                                segs[i + 2].Equals("explorer.exe.mui", StringComparison.OrdinalIgnoreCase))
                            {
                                string lang = segs[i + 1];
                                if (lang.Contains("-") || lang.Length == 5)
                                {
                                    if (detectedLang == null) detectedLang = lang;
                                }
                            }
                        }
                    }
                }

                if (imagePrefix == null) imagePrefix = "";
                if (detectedLang == null) detectedLang = "en-US";

                Log("[INFO] Detected WIM prefix: " + (imagePrefix == "" ? "(single-image root)" : imagePrefix) + " | Language: " + detectedLang);

                // Step 3: Extract explorer.exe using exact path
                Log("[INFO] Extracting explorer.exe...");
                string wimExplorerPath = imagePrefix + "Windows\\explorer.exe";
                RunProcess(sevenZip, string.Format("e \"{0}\" \"{1}\" -o\"{2}\" -y", wimPath, wimExplorerPath, outputDir));

                string destExplorer = Path.Combine(outputDir, "explorer.exe");
                if (File.Exists(destExplorer) && new FileInfo(destExplorer).Length > 0)
                {
                    Log("[OK] explorer.exe extracted (" + new FileInfo(destExplorer).Length + " bytes)");
                }
                else
                {
                    Log("[ERROR] explorer.exe could not be extracted from " + wimExplorerPath);
                    return false;
                }

                // Step 4: Extract language MUI files using exact paths
                string langDir = Path.Combine(outputDir, detectedLang);
                Directory.CreateDirectory(langDir);

                string wimExplorerMui = imagePrefix + "Windows\\" + detectedLang + "\\explorer.exe.mui";
                Log("[INFO] Extracting " + wimExplorerMui + "...");
                RunProcess(sevenZip, string.Format("e \"{0}\" \"{1}\" -o\"{2}\" -y", wimPath, wimExplorerMui, langDir));
                string destExplorerMui = Path.Combine(langDir, "explorer.exe.mui");
                Log(File.Exists(destExplorerMui) ? "[OK] explorer.exe.mui extracted." : "[ERROR] explorer.exe.mui missing.");

                string wimShell32Mui = imagePrefix + "Windows\\System32\\" + detectedLang + "\\shell32.dll.mui";
                Log("[INFO] Extracting " + wimShell32Mui + "...");
                RunProcess(sevenZip, string.Format("e \"{0}\" \"{1}\" -o\"{2}\" -y", wimPath, wimShell32Mui, langDir));
                string destShell32Mui = Path.Combine(langDir, "shell32.dll.mui");
                Log(File.Exists(destShell32Mui) ? "[OK] shell32.dll.mui extracted." : "[ERROR] shell32.dll.mui missing.");

                // Step 5: Extract Theme if requested using exact paths
                if (includeTheme)
                {
                    string themeDir = Path.Combine(outputDir, "theme");
                    string themeLangDir = Path.Combine(themeDir, detectedLang);
                    Directory.CreateDirectory(themeDir);
                    Directory.CreateDirectory(themeLangDir);

                    string wimAero = imagePrefix + "Windows\\Resources\\Themes\\Aero\\aero.msstyles";
                    Log("[INFO] Extracting " + wimAero + "...");
                    RunProcess(sevenZip, string.Format("e \"{0}\" \"{1}\" -o\"{2}\" -y", wimPath, wimAero, themeDir));

                    string extractedAero = Path.Combine(themeDir, "aero.msstyles");
                    string vistaTheme = Path.Combine(themeDir, "vista.msstyles");
                    if (File.Exists(extractedAero))
                    {
                        File.Copy(extractedAero, vistaTheme, true);
                        File.Delete(extractedAero);
                        Log("[OK] Theme file created: theme\\vista.msstyles");
                    }
                    else
                    {
                        Log("[ERROR] aero.msstyles could not be extracted.");
                    }

                    string wimAeroMui = imagePrefix + "Windows\\Resources\\Themes\\Aero\\" + detectedLang + "\\aero.msstyles.mui";
                    Log("[INFO] Extracting " + wimAeroMui + "...");
                    RunProcess(sevenZip, string.Format("e \"{0}\" \"{1}\" -o\"{2}\" -y", wimPath, wimAeroMui, themeLangDir));
                    string extractedMui = Path.Combine(themeLangDir, "aero.msstyles.mui");
                    string vistaMui = Path.Combine(themeLangDir, "vista.msstyles.mui");
                    if (File.Exists(extractedMui))
                    {
                        File.Copy(extractedMui, vistaMui, true);
                        File.Delete(extractedMui);
                        Log("[OK] Theme MUI created: theme\\" + detectedLang + "\\vista.msstyles.mui");
                    }
                    else
                    {
                        Log("[ERROR] aero.msstyles.mui could not be extracted.");
                    }
                }

                // Step 6: Automatically Patch PE imports if enabled
                if (autoPatch && File.Exists(destExplorer))
                {
                    Log("[INFO] Auto-patching PE import table in explorer.exe to use wrp64.dll...");
                    bool patchOk = PatchExplorerImports(destExplorer);
                    if (patchOk)
                    {
                        Log("[OK] explorer.exe successfully patched! (Backup saved as explorer.exe.bak)");
                    }
                    else
                    {
                        Log("[WARNING] Could not automatically patch PE imports.");
                    }
                }

                return true;
            }
            finally
            {
                if (!string.IsNullOrEmpty(tempDir) && Directory.Exists(tempDir))
                {
                    try { Directory.Delete(tempDir, true); } catch { }
                }
            }
        }

        private static int RvaToFileOffset(byte[] d, int rva, int numSections, int sectionHeadersOffset)
        {
            for (int i = 0; i < numSections; i++)
            {
                int sec = sectionHeadersOffset + (i * 40);
                int vSize = BitConverter.ToInt32(d, sec + 8);
                int vAddr = BitConverter.ToInt32(d, sec + 12);
                int rOff = BitConverter.ToInt32(d, sec + 20);
                if (rva >= vAddr && rva < vAddr + vSize)
                {
                    return rOff + (rva - vAddr);
                }
            }
            return -1;
        }

        private bool PatchExplorerImports(string exePath)
        {
            try
            {
                byte[] data = File.ReadAllBytes(exePath);

                // Create backup
                File.WriteAllBytes(exePath + ".bak", data);

                if (data.Length < 0x100 || BitConverter.ToUInt16(data, 0) != 0x5A4D)
                {
                    Log("[ERROR] Invalid MZ header.");
                    return false;
                }

                int e_lfanew = BitConverter.ToInt32(data, 0x3C);
                if (e_lfanew <= 0 || e_lfanew + 264 >= data.Length || BitConverter.ToUInt32(data, e_lfanew) != 0x00004550)
                {
                    Log("[ERROR] Invalid PE header.");
                    return false;
                }

                int optHeaderOffset = e_lfanew + 24;
                ushort magic = BitConverter.ToUInt16(data, optHeaderOffset);
                if (magic != 0x020B) // PE32+ (x64)
                {
                    Log("[ERROR] Expected 64-bit PE32+ executable.");
                    return false;
                }

                int numSections = BitConverter.ToInt16(data, e_lfanew + 6);
                int sizeOfOptionalHeader = BitConverter.ToInt16(data, e_lfanew + 20);
                int sectionHeadersOffset = optHeaderOffset + sizeOfOptionalHeader;
                int importTableRva = BitConverter.ToInt32(data, optHeaderOffset + 120); // DataDirectory[1]

                int importOffset = RvaToFileOffset(data, importTableRva, numSections, sectionHeadersOffset);
                if (importOffset < 0)
                {
                    Log("[ERROR] Could not locate PE Import Directory.");
                    return false;
                }

                string[] targets = { "SHLWAPI.DLL", "OLE32.DLL", "EXPLORERFRAME.DLL", "SHDOCVW.DLL" };
                byte[] replacement = Encoding.ASCII.GetBytes("wrp64.dll\0");

                int patchedCount = 0;
                int curr = importOffset;
                while (curr + 20 <= data.Length)
                {
                    int nameRva = BitConverter.ToInt32(data, curr + 12);
                    int firstThunk = BitConverter.ToInt32(data, curr + 16);
                    if (nameRva == 0 && firstThunk == 0) break;

                    int nameOffset = RvaToFileOffset(data, nameRva, numSections, sectionHeadersOffset);
                    if (nameOffset >= 0)
                    {
                        int end = nameOffset;
                        while (end < data.Length && data[end] != 0) end++;
                        int origLen = end - nameOffset;
                        string dllName = Encoding.ASCII.GetString(data, nameOffset, origLen);

                        foreach (var target in targets)
                        {
                            if (dllName.Equals(target, StringComparison.OrdinalIgnoreCase))
                            {
                                if (origLen + 1 >= replacement.Length)
                                {
                                    for (int k = 0; k <= origLen; k++)
                                    {
                                        data[nameOffset + k] = (k < replacement.Length) ? replacement[k] : (byte)0;
                                    }
                                    patchedCount++;
                                    Log(" -> Redirected PE import '" + dllName + "' (offset " + nameOffset + ") to 'wrp64.dll'");
                                }
                                else
                                {
                                    Log("[WARNING] Import name '" + dllName + "' too short to replace in-place.");
                                }
                                break;
                            }
                        }
                    }
                    curr += 20;
                }

                if (patchedCount > 0)
                {
                    File.WriteAllBytes(exePath, data);
                    return true;
                }
                return false;
            }
            catch (Exception ex)
            {
                Log("[ERROR] Patch error: " + ex.Message);
                return false;
            }
        }

        private string Find7Zip()
        {
            string[] paths = {
                @"C:\Program Files\7-Zip\7z.exe",
                @"C:\Program Files (x86)\7-Zip\7z.exe",
                Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "7z.exe")
            };

            foreach (var p in paths)
            {
                if (File.Exists(p)) return p;
            }
            return null;
        }

        private bool RunProcess(string exe, string args)
        {
            ProcessStartInfo psi = new ProcessStartInfo
            {
                FileName = exe,
                Arguments = args,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            using (Process proc = Process.Start(psi))
            {
                proc.WaitForExit();
                return proc.ExitCode == 0;
            }
        }

        private string RunProcessWithOutput(string exe, string args)
        {
            ProcessStartInfo psi = new ProcessStartInfo
            {
                FileName = exe,
                Arguments = args,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            using (Process proc = Process.Start(psi))
            {
                string outStr = proc.StandardOutput.ReadToEnd();
                proc.WaitForExit();
                return outStr;
            }
        }
    }
}
