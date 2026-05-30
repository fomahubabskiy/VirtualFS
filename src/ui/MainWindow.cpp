#include "MainWindow.h"
#include "../core/VirtualDiskManager.h"

#include <imgui.h>

VirtualDiskManager g_Manager;

bool MainWindow::Initialize()
{
    return true;
}

void MainWindow::Run()
{
    while (true)
    {
        ImGui::Begin("VirtualFS");

        if (ImGui::Button("Mount ISO"))
        {
            g_Manager.MountISO("test.iso");
        }

        if (ImGui::Button("Unmount"))
        {
            g_Manager.Unmount();
        }

        ImGui::Text("Mount Point: %s",
            g_Manager.GetMountPoint().c_str());

        ImGui::End();
    }
}

void MainWindow::Shutdown()
{}