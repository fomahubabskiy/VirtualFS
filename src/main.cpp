#include <filesystem>
#include <fstream>
#include <thread>
#include <atomic>
#include "iso/IsoReader.h"
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <windows.h>
#include <GLFW/glfw3.h>
#include <vector>
#include <string>
#include "fs/fs_bridge.h"
#include "fs/virtual_disk.h"
#include "iso/Extractor.h"
#include <shlobj.h>

namespace fs = std::filesystem;

void AddToStartup()
{
	HKEY hKey;
	RegOpenKeyExA(HKEY_CURRENT_USER,
		"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
		0, KEY_SET_VALUE, &hKey);

	char exePath[MAX_PATH];
	GetModuleFileNameA(NULL, exePath, MAX_PATH);

	std::string cmd = std::string("\"") + exePath + "\" --service";

	RegSetValueExA(hKey,
		"VirtualFS",
		0,
		REG_SZ,
		(BYTE*)cmd.c_str(),
		cmd.size() + 1);

	RegCloseKey(hKey);
}

void StartServiceProcess()
{
	char exePath[MAX_PATH];
	GetModuleFileNameA(NULL, exePath, MAX_PATH);

	std::string cmd = std::string("\"") + exePath + "\" --service";

	STARTUPINFOA si = { sizeof(si) };
	PROCESS_INFORMATION pi;

	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE; // скрыть окно

	CreateProcessA(
		NULL,
		(LPSTR)cmd.c_str(),
		NULL, NULL, FALSE,
		CREATE_NO_WINDOW, // запуск без консоли
		NULL, NULL,
		&si, &pi
	);

	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
}

bool IsDiskMounted()
{
	return GetFileAttributesA("X:\\") != INVALID_FILE_ATTRIBUTES;
}

bool IsoExists(const std::vector<std::string>& list, const std::string& path)
{
	for (auto& p : list)
		if (_stricmp(p.c_str(), path.c_str()) == 0)
			return true;
	return false;
}

std::atomic<float> progress = 0.0f;
std::atomic<bool> fsRunning = false;
float displayProgress = 0.0f; // плавный прогресс
std::string mountedPath = "";
std::string lastMountFile = "C:\\VirtualFSRoot\\last_mount.txt";

enum class SourceType
{
	None,
	ISO,
	ZIP,
	ANY
};

SourceType currentSource = SourceType::None;

std::atomic<float> mountProgress = 0.0f;
std::atomic<float> extractProgress = 0.0f;
std::atomic<bool> isWorking = false;
std::atomic<bool> isExtracting = false;
std::atomic<bool> isMounted = false;
std::atomic<bool> fsBusy = false;

void MountArchiveSafe(const std::string& path)
{
	// обнуление прогресса чтобы gui не лагал
	std::ofstream clear("C:\\VirtualFSRoot\\progress.txt", std::ios::trunc);
	clear << 0.0f;
	clear.close();

	// для прогресса
	if (isWorking) return;
	isWorking = true;
	progress = 0.0f;

	// id маунта = новая задача
	int mountId = GetTickCount64();

	CreateDirectoryA("C:\\VirtualFSRoot", NULL);

	// вечный дебаг1
	std::ofstream dbg1("C:\\VirtualFSRoot\\debug.txt", std::ios::app);
	dbg1 << "WRITE: " << mountId << " " << path << "\n";

	// запись команды сервису
	std::ofstream f(lastMountFile, std::ios::trunc);
	if (f.is_open())
	{
		f << mountId << "|ANY|" << path << std::endl;
		f.flush();
	}

	// сброс прогресса
	mountProgress = 0.0f;
	displayProgress = 0.0f;


	// ? тип файла
	mountedPath = path;

	// ждем сервис
	isMounted = false;
}


void CreateStartupTask()
{
	char exePath[MAX_PATH];
	GetModuleFileNameA(NULL, exePath, MAX_PATH);

	std::string cmd =
		"schtasks /create /tn VirtualFS "
		"/tr \"" + std::string(exePath) + " --service\" "
		"/sc onlogon "
		"/rl highest "
		"/f";

	system(cmd.c_str());
}
bool IsServiceRunning()
{
	FILE* pipe = _popen("tasklist /FI \"COMMANDLINE eq *--service*\"", "r");
	if (!pipe) return false;

	char buffer[256];
	std::string result;

	while (fgets(buffer, sizeof(buffer), pipe))
		result += buffer;

	_pclose(pipe);

	return result.find("--service") != std::string::npos;
}

int main(int argc, char** argv)
{
	if (argc == 1) // GUI режим
	{
		HANDLE hMutex = OpenMutexA(MUTEX_ALL_ACCESS, FALSE, "Global\\VirtualFS_Service");

		if (!hMutex)
		{
			// сервиса нет => запускаем
			StartServiceProcess();
			Sleep(1000);
		}
		else
		{
			CloseHandle(hMutex);
		}
	}

	// SERVICE MODE
	if (argc > 1 && strcmp(argv[1], "--service") == 0)
	{
		HANDLE hMutex = CreateMutexA(NULL, TRUE, "Global\\VirtualFS_Service");

		if (GetLastError() == ERROR_ALREADY_EXISTS)
		{
			return 0; // уже есть сервис
		}

		std::ofstream dbg("C:\\VirtualFSRoot\\service_start.txt");
		dbg << "SERVICE STARTED\n";

		CreateDirectoryW(L"C:\\VirtualFSRoot", NULL);
		Sleep(2000);

		int lastId = -1;

		while (true)
		{
			std::ofstream dbg2("C:\\VirtualFSRoot\\alive.txt", std::ios::app);
			dbg2 << "tick\n";

			std::ifstream f(lastMountFile);
			if (!f.is_open())
			{
				Sleep(500);
				continue;
			}

			std::string line;
			std::getline(f, line);
			f.close();

			if (line.find("|UNMOUNT|") != std::string::npos)
			{
				StopFileSystem();
				g_VFS.Clear();

				// прогресс = 0 
				std::ofstream prog("C:\\VirtualFSRoot\\progress.txt");
				prog << 0.0f;

				continue;
			}

			std::ofstream dbg3("C:\\VirtualFSRoot\\service.txt", std::ios::app);
			dbg3 << "READ: " << line << "\n";

			if (line.empty())
			{
				Sleep(500);
				continue;
			}

			// parse id
			size_t first = line.find('|');
			if (first == std::string::npos)
			{
				Sleep(300);
				continue;
			}

			int id = atoi(line.substr(0, first).c_str());

			if (id == lastId)
			{
				Sleep(300);
				continue;
			}

			lastId = id;

			// parse path
			size_t second = line.find('|', first + 1);
			if (second == std::string::npos)
			{
				Sleep(300);
				continue;
			}
			std::string path = line.substr(second + 1);

			// если mount тогда не mount
			if (IsDiskMounted() && path == mountedPath)
			{
				std::ofstream log("C:\\VirtualFSRoot\\skip.txt", std::ios::app);
				log << "SKIP: already mounted\n";
				Sleep(500);
				continue;
			}

			// стопнуть старую FS
			StopFileSystem();
			Sleep(500);
			g_VFS.Clear();

			// ЖДЕМ ОСВОБОЖДЕНИЯ ДИСКА
			for (int i = 0; i < 50; i++)
			{
				if (!IsDiskMounted())
					break;
				Sleep(200);
			}
			Sleep(300); 

			// LOAD ISO
			g_VFS.Clear();

			IsoReader iso;
			if (!iso.Open(path))
			{
				Sleep(500);
				continue;
			}

			std::ofstream prog("C:\\VirtualFSRoot\\progress.txt");
			prog << 0.1f;
			prog.close();

			iso.LoadToVFS(g_VFS);

			std::ofstream prog2("C:\\VirtualFSRoot\\progress.txt");
			prog2 << 0.6f;
			prog2.close();

			std::ofstream log("C:\\VirtualFSRoot\\before_start.txt", std::ios::app);
			log << "TRY START: " << path << "\n";

			// START FSretry
			bool started = false;

			for (int i = 0; i < 30; i++)
			{
				if (StartFileSystem(L"C:\\VirtualFSRoot"))
				{
					started = true;
					break;
				}
				Sleep(500);
			}

			std::ofstream log1("C:\\VirtualFSRoot\\fs_result.txt", std::ios::app);
			log1 << "START RESULT: " << started << " PATH: " << path << "\n";

			if (!started)
			{
				continue;
			}

			std::ofstream prog3("C:\\VirtualFSRoot\\progress.txt");
			prog3 << 1.0;
			prog3.close();

			// СОХРАНЯЕМ СТАТУС
			std::ofstream status("C:\\VirtualFSRoot\\status.txt", std::ios::trunc);
			status << path;
			status.close();

			if (!started)
			{
				DWORD err = GetLastError();

				// лог вместо MessageBox (в сервисе нельзя!)
				std::ofstream log("C:\\VirtualFSRoot\\error.txt", std::ios::app);
				log << "FS START FAIL for: " << path << "\n";
				continue;
			}

			// SUCCESS
			isMounted = true;
			mountedPath = path;

			Sleep(300);
		}
	}

	// GUI MODE
	char* userProfile = nullptr;
	size_t len;
	_dupenv_s(&userProfile, &len, "USERPROFILE");

	std::string basePath = userProfile;
	free(userProfile);

	std::vector<std::string> searchPaths = {
	basePath + "\\Downloads",
	basePath + "\\Documents",
	basePath + "\\Desktop",
	};
	std::vector<std::string> isoList;

	for (const auto& root : searchPaths)
		try
		{
			for (const auto& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied))
			{
				if (!entry.is_regular_file()) continue;

				if (entry.path().extension() == ".iso")
				{
					std::string full = entry.path().string();
					if (full.find(".zip") != std::string::npos || full.find(".rar") != std::string::npos)
						continue;

					isoList.push_back(full);
				}
			}
		}
		catch (...) {}

		IsoReader iso;
		std::vector<std::string> isoFiles;
		std::string selectedIso = "";
		char manualPath[512] = "";
		char extractPath[512] = "C:\\ExtractedISO";

		char zipPath[512] = "";
		char anyPath[512] = "";
		bool anyMounted = false;

		// GLFW
		if (!glfwInit()) return -1;
		GLFWwindow* window = glfwCreateWindow(1280, 720, "VirtualFS", NULL, NULL);

		if (!window) return -1;
		glfwMakeContextCurrent(window);

		// IMGUI
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGui::StyleColorsDark();
		ImGui_ImplGlfw_InitForOpenGL(window, true);
		ImGui_ImplOpenGL3_Init("#version 130");

		while (!glfwWindowShouldClose(window))
		{
			glfwPollEvents();
			ImGui_ImplOpenGL3_NewFrame();
			ImGui_ImplGlfw_NewFrame();

			// ЧИТАЕМ СТАТУС ОТ СЕРВИСА
			std::ifstream s("C:\\VirtualFSRoot\\status.txt");
			if (s.is_open())
			{
				std::string p;
				std::getline(s, p);

				if (!p.empty())
				{
					isMounted = true;
					mountedPath = p;
				}
				else
				{
					isMounted = false;
					mountedPath.clear();
				}
			}
			else
			{
				isMounted = false;
				mountedPath.clear();
			}

			std::ifstream pfile("C:\\VirtualFSRoot\\progress.txt");
			if (pfile.is_open())
			{
				float p;
				pfile >> p;
				mountProgress = p;
			}

			// попытка 8976756е532467 плавного прогресса
			float target = isExtracting ? extractProgress : mountProgress;
			if (target < displayProgress)
			{
				displayProgress = target; 
			}
			else
			{
				displayProgress += (target - displayProgress) * 0.05f;
			}

			// mount / extract завершён
			if (!isExtracting && mountProgress >= 1.0f)
			{
				isWorking = false;
				displayProgress = 1.0f;
			}

			ImGui::NewFrame();
			ImGui::Begin("VirtualFS");

			// лочим все пока идет mount
			ImGui::BeginDisabled(isWorking || fsBusy);

			if (ImGui::BeginTabBar("MainTabs"))
			{
				// ISO TAB
				if (ImGui::BeginTabItem("ISO"))
				{
					// рефрешшшш
					if (ImGui::Button("Refresh ISO"))
					{
						isoList.clear();
						for (const auto& root : searchPaths)
						{
							try
							{
								for (const auto& entry : fs::recursive_directory_iterator(
									root, fs::directory_options::skip_permission_denied))
								{
									if (!entry.is_regular_file()) continue;
									if (entry.path().extension() == ".iso")
									{
										isoList.push_back(entry.path().string());
									}
								}
							}
							catch (...) {}
						}
					}
					ImGui::Text("Selected ISO:");
					ImGui::Text(selectedIso.c_str());

					ImGui::Separator();

					ImGui::Text("Found ISO:");
					for (auto& isoPath : isoList)
					{

						bool selected = (isoPath == selectedIso);
						if (selected)
							ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 255, 0, 255));

						if (ImGui::Selectable(isoPath.c_str(), selected))
						{
							selectedIso = isoPath;
							zipPath[0] = 0;
							anyPath[0] = 0;
							currentSource = SourceType::ISO;
						}
						if (selected)
							ImGui::PopStyleColor();
					}
					ImGui::Separator();
					if (ImGui::Button("Select ISO manually"))
					{
						char fileName[MAX_PATH] = "";
						OPENFILENAMEA ofn = {};
						ofn.lStructSize = sizeof(ofn);
						ofn.lpstrFilter = "ISO Files\0*.iso\0";
						ofn.lpstrFile = fileName;
						ofn.nMaxFile = MAX_PATH;

						if (GetOpenFileNameA(&ofn))
						{
							if (IsoExists(isoList, fileName))
							{
								MessageBoxA(NULL, "ISO already added", "Warning", MB_OK);
							}
							else
							{
								isoList.push_back(fileName);
							}
						}
					}
					ImGui::BeginDisabled(isWorking || fsBusy);

					if (ImGui::Button("Mount ISO") && !isWorking && !fsBusy)
					{
						if (selectedIso.empty())
							MessageBoxA(NULL, "No ISO selected", "ERROR", MB_OK);
						else
						{
							if (isMounted && _stricmp(mountedPath.c_str(), selectedIso.c_str()) == 0)
							{
								// не mount если уже mount 
								MessageBoxA(NULL, "Already mounted", "INFO", MB_OK);
							}
							else
							{
								// mount
								progress = 0.0f;
								displayProgress = 0.0f;
								mountProgress = 0.0f;
								MountArchiveSafe(selectedIso);
							}
						}
					}

					if (ImGui::Button("Unmount") && !isWorking)
					{
						if (!isMounted)
						{
							MessageBoxA(NULL, "Nothing mounted", "INFO", MB_OK);
						}
						else
						{
							std::ofstream f(lastMountFile, std::ios::trunc);
							f << GetTickCount64() << "|UNMOUNT|" << std::endl;
							isWorking = true;

							std::thread([]()
								{
									StopFileSystem();
									g_VFS.Clear();
									isMounted = false;
									mountedPath.clear();
									progress = 1.0f;
									isWorking = false;
								}).detach();
						}
					}
					ImGui::EndDisabled();
					ImGui::EndTabItem();
				}
				// ZIP TAB
				// список найденных ZIP 
				static std::vector<std::string> zipList;

				if (ImGui::BeginTabItem("ZIP"))
				{
					// рефреш
					if (ImGui::Button("Refresh ZIP"))
					{
						zipList.clear();
						for (const auto& root : searchPaths)
						{
							try
							{
								for (const auto& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied))
								{
									if (!entry.is_regular_file()) continue;
									if (entry.path().extension() == ".zip")
									{
										zipList.push_back(entry.path().string());
									}
								}
							}
							catch (...) {}
						}
					}
					ImGui::Text("Selected ZIP:");
					ImGui::Text(zipPath);
					ImGui::Separator();

					if (zipList.empty())
					{
						for (const auto& root : searchPaths)
							try
							{
								for (const auto& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied))
								{
									if (!entry.is_regular_file()) continue;
									if (entry.path().extension() == ".zip")
									{
										zipList.push_back(entry.path().string());
									}
								}
							}
							catch (...) {}
					}
					ImGui::Text("Found ZIP:");
					for (auto& z : zipList)
					{
						bool selected = (_stricmp(z.c_str(), zipPath) == 0);
						if (selected)
							ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 255, 0, 255));
						if (ImGui::Selectable(z.c_str(), selected))
						{
							strcpy_s(zipPath, sizeof(zipPath), z.c_str());
							selectedIso.clear();
							anyPath[0] = 0;
							currentSource = SourceType::ZIP;
						}
						if (selected)
							ImGui::PopStyleColor();
					}
					ImGui::Separator();

					// ручной выбор
					if (ImGui::Button("Select ZIP manually"))
					{
						char fileName[MAX_PATH] = "";
						OPENFILENAMEA ofn = {};
						ofn.lStructSize = sizeof(ofn);
						ofn.lpstrFilter = "ZIP Files\0*.zip\0";
						ofn.lpstrFile = fileName;
						ofn.nMaxFile = MAX_PATH;

						if (GetOpenFileNameA(&ofn))
						{
							strcpy_s(zipPath, sizeof(zipPath), fileName);
							selectedIso.clear();
							anyPath[0] = 0;
							currentSource = SourceType::ZIP;
						}
					}
					ImGui::BeginDisabled(isWorking || fsBusy);

					if (ImGui::Button("Mount ZIP") && !isWorking && !fsBusy)
					{
						if (strlen(zipPath) == 0)
							MessageBoxA(NULL, "No ZIP selected", "ERROR", MB_OK);
						else
						{
							if (isMounted && _stricmp(mountedPath.c_str(), zipPath) == 0)
							{
								MessageBoxA(NULL, "Already mounted", "INFO", MB_OK);
							}
							else
							{
								displayProgress = 0.0f;
								mountProgress = 0.0f;
								MountArchiveSafe(zipPath);
							}
						}
					}

					if (ImGui::Button("Unmount") && !isWorking)
					{
						if (!isMounted)
						{
							MessageBoxA(NULL, "Nothing mounted", "INFO", MB_OK);
						}
						else
						{
							std::ofstream f(lastMountFile, std::ios::trunc);
							f << GetTickCount64() << "|UNMOUNT|" << std::endl;
							isWorking = true;

							std::thread([]()
								{
									StopFileSystem();
									g_VFS.Clear();

									isMounted = false;
									progress = 1.0f;
									isWorking = false;

								}).detach();
						}
					}

					ImGui::EndDisabled();

					ImGui::EndTabItem();
				}

				
				// ANY TAB
				
				if (ImGui::BeginTabItem("ANY"))
				{
					ImGui::Text("Open any archive");

					ImGui::InputText("Path", anyPath, sizeof(anyPath));

					if (ImGui::Button("Select file"))
					{
						char fileName[MAX_PATH] = "";
						OPENFILENAMEA ofn = {};
						ofn.lStructSize = sizeof(ofn);
						ofn.lpstrFilter = "All Files\0*.*\0";
						ofn.lpstrFile = fileName;
						ofn.nMaxFile = MAX_PATH;

						if (GetOpenFileNameA(&ofn))
						{
							strcpy_s(anyPath, sizeof(anyPath), fileName);

							selectedIso.clear();
							zipPath[0] = 0;

							currentSource = SourceType::ANY;
						}
					}

					ImGui::Separator();

					ImGui::BeginDisabled(isWorking || fsBusy);

					if (ImGui::Button("Mount ANY") && !isWorking && !fsBusy)
					{
						if (strlen(anyPath) == 0)
							MessageBoxA(NULL, "No file selected", "ERROR", MB_OK);
						else
						{
							if (isMounted && _stricmp(mountedPath.c_str(), anyPath) == 0)
							{
								MessageBoxA(NULL, "Already mounted", "INFO", MB_OK);
							}
							else
							{
								displayProgress = 0.0f;
								mountProgress = 0.0f;
								MountArchiveSafe(anyPath);
							}
						}
					}

					if (ImGui::Button("Unmount") && !isWorking)
					{
						if (!isMounted)
						{
							MessageBoxA(NULL, "Nothing mounted", "INFO", MB_OK);
						}
						else
						{
							std::ofstream f(lastMountFile, std::ios::trunc);
							f << GetTickCount64() << "|UNMOUNT|" << std::endl;
							isWorking = true;

							std::thread([]()
								{
									StopFileSystem();
									g_VFS.Clear();

									isMounted = false;
									progress = 1.0f;
									isWorking = false;

								}).detach();
						}
					}

					ImGui::EndDisabled();

					ImGui::EndTabItem();
				}

				ImGui::EndTabBar();
			}

			// EXTRACT для всего
			ImGui::Separator();

			static char outPath[260] = "C:\\ExtractedISO";
			ImGui::InputText("Output Folder", outPath, sizeof(outPath));

			if (ImGui::Button("Extract ALL"))
			{
				if (!isMounted || mountedPath.empty())
				{
					MessageBoxA(NULL, "Nothing mounted", "ERROR", MB_OK);
				}
				else
				{
					std::string outCopy = outPath;
					std::string inputPath = mountedPath; // ВСЕГДА берем mount файл

					progress = 0.0f;
					isExtracting = true;
					isWorking = true;

					extractProgress = 0.0f;
					displayProgress = 0.0f;

					std::thread([inputPath, outCopy]()
						{
							std::string name = fs::path(inputPath).stem().string();
							std::string finalPath = outCopy + "\\" + name;

							CreateDirectoryA(finalPath.c_str(), NULL);

							ExtractAll(inputPath, finalPath, extractProgress);

							extractProgress = 1.0f;
							Sleep(500); // показать 100%

							isExtracting = false;
							isWorking = false;
						}).detach();
				}
			}

			ImGui::EndDisabled();
			ImGui::Separator();

			// прогресс
			if (isWorking || isExtracting)
			{
				ImGui::ProgressBar(displayProgress, ImVec2(-1, 20));
				ImGui::Text("Working...");
			}

			ImGui::Separator();

			// список файлов
			for (auto& f : isoFiles)
			{
				ImGui::BulletText(f.c_str());
			}

			// кто в диске?
			ImGui::Separator();

			if (isMounted)
			{
				ImGui::Text("Active: %s", mountedPath.c_str());
			}
			else
			{
				ImGui::Text("Active: NONE");
			}

			// открыть диск
			if (ImGui::Button("Open X:"))
			{
				system("explorer X:");
			}

			ImGui::End();

			ImGui::Render();
			int w, h;
			glfwGetFramebufferSize(window, &w, &h);
			glViewport(0, 0, w, h);
			glClear(GL_COLOR_BUFFER_BIT);
			ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
			glfwSwapBuffers(window);
		}

		return 0;
}
