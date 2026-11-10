
    const loginForm = document.getElementById("login-form");
    const loginPage = document.getElementById("login-page");
    const dashboardPage = document.getElementById("dashboard-page");
    const logoutLink = document.getElementById("logout-link");
    const navLinks = document.querySelectorAll(".menu a[data-view]");
    const viewLinks = document.querySelectorAll("[data-view]");
    const scheduleForm = document.getElementById("schedule-form");
    const scheduleBody = document.getElementById("schedule-body");
    const scheduleListBody = document.getElementById("schedule-list-body");
    const editIdInput = document.getElementById("edit-id");
    const scheduleTimeInput = document.getElementById("schedule-time");
    const schedulePeriodInput = document.getElementById("schedule-period");
    const scheduleDescriptionInput = document.getElementById("schedule-description");
    const scheduleAudioInput = document.getElementById("schedule-audio");
    const scheduleAudioSelect = document.getElementById("schedule-audio-select");
    const cancelEditBtn = document.getElementById("cancel-edit-btn");
    const scheduleMessage = document.getElementById("schedule-message");
    const currentTimeDisplay = document.getElementById("current-time");
    const nextBellDisplay = document.getElementById("next-bell");
    const scheduleNextBellDisplay = document.getElementById("schedule-next-bell");
    const batteryStatusDisplay = document.getElementById("battery-status");
    const systemModeDisplay = document.getElementById("system-mode");
    const manualOverrideBtn = document.getElementById("manual-override-btn");
    const emergencyBtn = document.getElementById("emergency-btn");
    const backupBtn = document.getElementById("backup-btn");
    const actionMessage = document.getElementById("action-message");
    const scheduleCountDisplay = document.getElementById("schedule-count");
    const settingsForm = document.getElementById("settings-form");
    const settingsDisplayName = document.getElementById("settings-display-name");
    const settingsAvatar = document.getElementById("settings-avatar");
    const settingsAvatarPreview = document.getElementById("settings-avatar-preview");
    const settingsEmail = document.getElementById("settings-email");
    const settingsPhone = document.getElementById("settings-phone");
    const settingsBio = document.getElementById("settings-bio");
    const settingsPassword = document.getElementById("settings-password");
    const settingsPasswordConfirm = document.getElementById("settings-password-confirm");
    const settingsRecoveryEmail = document.getElementById("settings-recovery-email");
    const settingsRecoveryPhone = document.getElementById("settings-recovery-phone");
    const settings2fa = document.getElementById("settings-2fa");
    const settingsEmailNotifications = document.getElementById("settings-email-notifications");
    const settingsPushNotifications = document.getElementById("settings-push-notifications");
    const settingsAppearance = document.getElementById("settings-appearance");
    const settingsLanguage = document.getElementById("settings-language");
    const settingsDefaultPeriod = document.getElementById("settings-default-period");
    const settingsMessage = document.getElementById("settings-message");

    const defaultSchedules = [
      { id: 1, time: "07:45 AM", description: "Flag Ceremony", audio: "anthem.mp3" },
      { id: 2, time: "08:30 AM", description: "Start of Class", audio: "bell.mp3" },
      { id: 3, time: "10:00 AM", description: "Recess", audio: "recess.mp3" }
    ];
    let schedules;
    try {
      const savedSchedules = JSON.parse(localStorage.getItem("fcuBellSchedules") || "null");
      schedules = Array.isArray(savedSchedules)
        ? savedSchedules.map(({ audioData, ...schedule }) => schedule)
        : defaultSchedules;
    } catch {
      schedules = defaultSchedules;
      localStorage.removeItem("fcuBellSchedules");
    }
    let editingId = null;
    let highlightedScheduleId = null;

    function showDashboard() {
      loginPage.style.display = "none";
      dashboardPage.style.display = "flex";
      localStorage.setItem("fcuBellLoggedIn", "true");
    }

    function showLogin() {
      loginPage.style.display = "flex";
      dashboardPage.style.display = "none";
      localStorage.removeItem("fcuBellLoggedIn");
    }

    function showView(viewId) {
      document.querySelectorAll(".view-section").forEach((section) => {
        section.classList.toggle("active", section.id === viewId);
      });

      navLinks.forEach((link) => {
        link.classList.toggle("active", link.getAttribute("data-view") === viewId);
      });
    }

    function resetScheduleForm() {
      scheduleForm.reset();
      editIdInput.value = "";
      editingId = null;
      scheduleMessage.textContent = "";
    }

    function saveSchedules() {
      schedules = schedules.map(({ audioData, ...schedule }) => schedule);
      localStorage.setItem("fcuBellSchedules", JSON.stringify(schedules));
    }

    function renderSchedules() {
      scheduleBody.innerHTML = "";
      scheduleListBody.innerHTML = "";

      if (schedules.length === 0) {
        scheduleBody.innerHTML = '<tr><td colspan="4" class="empty-state">No bell schedules yet.</td></tr>';
        scheduleListBody.innerHTML = '<tr><td colspan="3" class="empty-state">No bell schedules yet.</td></tr>';
        saveSchedules();
        updateSummary();
        return;
      }

      schedules.forEach((item) => {
        const row = document.createElement("tr");
        row.id = `schedule-row-${item.id}`;
        if (item.id === highlightedScheduleId) row.classList.add("new-schedule-row");
        row.innerHTML = `
          <td>${item.time}</td>
          <td>${item.description}</td>
          <td>${item.audio}</td>
          <td>
            <button type="button" class="edit-btn" data-id="${item.id}">Edit</button>
            <button type="button" class="delete-btn" data-id="${item.id}">Delete</button>
          </td>
        `;
        scheduleBody.appendChild(row);

        const listRow = document.createElement("tr");
        listRow.innerHTML = `
          <td>${item.time}</td>
          <td>${item.description}</td>
          <td>${item.audio}</td>
        `;
        scheduleListBody.appendChild(listRow);
      });

      saveSchedules();
      updateSummary();
    }

    function updateSummary() {
      if (schedules.length > 0) {
        const next = schedules[0];
        nextBellDisplay.textContent = `${next.time} â€“ ${next.description}`;
        scheduleNextBellDisplay.textContent = `${next.time} â€“ ${next.description}`;
      } else {
        nextBellDisplay.textContent = "No schedule set";
        scheduleNextBellDisplay.textContent = "No schedule set";
      }

      scheduleCountDisplay.textContent = schedules.length;
    }

    function showMessage(message, type = "success") {
      scheduleMessage.textContent = message;
      scheduleMessage.className = `message ${type}`;
    }

    function loadSettings() {
      let settings;
      try {
        settings = JSON.parse(localStorage.getItem("fcuBellSettings") || "null") || {};
      } catch {
        settings = {};
      }
      settings = {
        displayName: "ADMIN",
        email: "",
        phone: "",
        bio: "",
        avatar: "pfp.jpg",
        recoveryEmail: "",
        recoveryPhone: "",
        twoFactor: false,
        emailNotifications: true,
        pushNotifications: false,
        appearance: "light",
        language: "en",
        defaultPeriod: "AM",
        ...settings
      };
      settingsDisplayName.value = settings.displayName;
      settingsEmail.value = settings.email;
      settingsPhone.value = settings.phone;
      settingsBio.value = settings.bio;
      settingsAvatarPreview.src = settings.avatar;
      document.getElementById("profile-avatar").src = settings.avatar;
      settingsRecoveryEmail.value = settings.recoveryEmail;
      settingsRecoveryPhone.value = settings.recoveryPhone;
      settings2fa.checked = settings.twoFactor;
      settingsEmailNotifications.checked = settings.emailNotifications;
      settingsPushNotifications.checked = settings.pushNotifications;
      settingsAppearance.value = settings.appearance;
      settingsLanguage.value = settings.language;
      settingsDefaultPeriod.value = settings.defaultPeriod;
      document.body.dataset.appearance = settings.appearance;
    }

    settingsAvatar.addEventListener("change", () => {
      const file = settingsAvatar.files[0];
      if (!file) return;
      const reader = new FileReader();
      reader.onload = () => {
        const image = new Image();
        image.onload = () => {
          const canvas = document.createElement("canvas");
          const scale = Math.min(1, 256 / Math.max(image.width, image.height));
          canvas.width = Math.max(1, Math.round(image.width * scale));
          canvas.height = Math.max(1, Math.round(image.height * scale));
          canvas.getContext("2d").drawImage(image, 0, 0, canvas.width, canvas.height);
          settingsAvatarPreview.src = canvas.toDataURL("image/jpeg", 0.8);
        };
        image.src = reader.result;
      };
      reader.readAsDataURL(file);
    });

    window.addEventListener("error", (event) => {
      if (event.error) showMessage(`Schedule error: ${event.error.message}`, "error");
    });

    function showActionMessage(message, type = "success") {
      actionMessage.textContent = message;
      actionMessage.className = `message ${type}`;
    }

    function updateDashboardStatus() {
      const now = new Date();
      currentTimeDisplay.textContent = now.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
    }

    function formatScheduleTime(time, period) {
      const cleanedTime = time.trim();
      const match = cleanedTime.match(/^(\d{1,2}):?(\d{2})$/);
      if (!match) return "";

      const hours = Number(match[1]);
      const minutes = Number(match[2]);
      if (hours > 23 || minutes > 59) return "";
      let displayHours = hours % 12 || 12;
      return `${String(displayHours).padStart(2, "0")}:${String(minutes).padStart(2, "0")} ${period}`;
    }

    function loadScheduleTime(value) {
      const match = value.match(/^(\d{1,2}):(\d{2})\s*(AM|NN|PM)$/i);
      if (!match) return;

      let hours = Number(match[1]);
      const minutes = match[2];
      const period = match[3].toUpperCase();
      if (period === "PM" && hours < 12) hours += 12;
      if (period === "AM" && hours === 12) hours = 0;
      scheduleTimeInput.value = `${String(hours).padStart(2, "0")}:${minutes}`;
      schedulePeriodInput.value = period;
    }

    async function addOrUpdateSchedule(e) {
      e.preventDefault();
      let formattedTime;
      let description;
      let audio;
      try {
        const time = scheduleTimeInput.value;
        const period = schedulePeriodInput?.value || "AM";
        formattedTime = time ? formatScheduleTime(time, period) : "";
        description = scheduleDescriptionInput.value.trim();
        const selectedAudio = scheduleAudioSelect?.value || "";
        const existingAudio = editingId ? schedules.find((item) => item.id === editingId)?.audio : "";

        if (!formattedTime || !description) {
          showMessage("Please provide a valid time, period, and description.", "error");
          return;
        }

        audio = selectedAudio || existingAudio || "No audio selected";
      } catch (error) {
        showMessage(`Could not save schedule: ${error.message}`, "error");
        return;
      }

      try {
        const wasEditing = Boolean(editingId);
        highlightedScheduleId = editingId || Date.now();

        if (editingId) {
          schedules = schedules.map((item) =>
            item.id === editingId ? { ...item, time: formattedTime, description, audio } : item
          );
        } else {
          schedules.unshift({ id: highlightedScheduleId, time: formattedTime, description, audio });
        }

        saveSchedules();
        renderSchedules();
        resetScheduleForm();
        showMessage(wasEditing ? "Schedule updated successfully." : "Schedule added successfully.");
        const visibleRow = document.getElementById(`schedule-row-${highlightedScheduleId}`);
        if (visibleRow) visibleRow.scrollIntoView({ behavior: "smooth", block: "center" });
      } catch (error) {
        showMessage(`Could not save schedule: ${error.message}`, "error");
      }
    }

    function startEditSchedule(id) {
      const item = schedules.find((entry) => entry.id === id);
      if (!item) return;

      editingId = id;
      editIdInput.value = id;
      loadScheduleTime(item.time);
      scheduleDescriptionInput.value = item.description;
      if (scheduleAudioSelect) scheduleAudioSelect.value = item.audio;
      scheduleTimeInput.focus();
      showMessage("Editing schedule entry.");
      showView("dashboard-view");
    }

    function deleteSchedule(id) {
      schedules = schedules.filter((item) => item.id !== id);
      if (editingId === id) {
        resetScheduleForm();
      }
      renderSchedules();
      showMessage("Schedule deleted successfully.");
    }

    function triggerManualOverride() {
      systemModeDisplay.textContent = "Manual Override";
      if (batteryStatusDisplay) batteryStatusDisplay.textContent = "92% (Active)";
      showActionMessage("Manual bell override triggered successfully.");
      logEvent("Manual override executed");
    }

    function triggerEmergencyDetection() {
      systemModeDisplay.textContent = "Emergency";
      if (batteryStatusDisplay) batteryStatusDisplay.textContent = "100% (Backup)";
      showActionMessage("Emergency alert mode activated.");
      logEvent("Emergency detection activated");
    }

    function backupAndRestore() {
      saveSchedules();
      showActionMessage("Backup restored and schedules saved.");
      logEvent("Backup and restore completed");
    }

    function logEvent(message) {
      const logs = JSON.parse(localStorage.getItem("fcuBellLogs") || "[]");
      logs.unshift({ time: new Date().toLocaleString(), message });
      localStorage.setItem("fcuBellLogs", JSON.stringify(logs.slice(0, 20)));
    }

    async function loadUsers() {
      try {
        const r = await fetch("/api/users");
        if (!r.ok) return;
        const data = await r.json();
        const users = data.users || [];
        const body = document.getElementById("user-body");
        if (!body) return;
        body.innerHTML = users.map((u) => `
          <tr>
            <td>${u.name}</td>
            <td>${u.role}</td>
            <td><span class="status-badge ${u.status === "Active" ? "active" : "pending"}">${u.status}</span></td>
            <td>${u.lastLogin}</td>
            <td>
              ${u.status !== "Active" ? `<button class="approve-user-btn" data-name="${u.name}">Approve</button>` : ""}
              <button class="delete-user-btn" data-name="${u.name}">Delete</button>
            </td>
          </tr>`).join("");
      } catch (e) {}
    }

    async function loadAudioFiles() {
      try {
        const r = await fetch("/api/audio-files");
        if (!r.ok) return;
        const data = await r.json();
        const files = data.files || [];
        if (scheduleAudioSelect) {
          const current = scheduleAudioSelect.value;
          scheduleAudioSelect.innerHTML = '<option value="">-- select audio --</option>' +
            files.map((f) => `<option value="${f}">${f}</option>`).join("");
          if (current) scheduleAudioSelect.value = current;
        }
      } catch (e) {}
    }

    async function upsertUser(payload) {
      try {
        await fetch("/api/users", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(payload) });
        loadUsers();
      } catch (e) {}
    }

    document.getElementById("add-user-btn")?.addEventListener("click", async () => {
      const name = prompt("User name:");
      if (!name) return;
      const role = prompt("Role (Administrator/Staff/Viewer):", "Staff") || "Staff";
      await upsertUser({ name, role, status: "Active", lastLogin: "Just now" });
    });

    document.addEventListener("click", async (e) => {
      const approveBtn = e.target.closest(".approve-user-btn");
      if (approveBtn) {
        const name = approveBtn.getAttribute("data-name");
        await upsertUser({ name, status: "Active", lastLogin: "Just now" });
        return;
      }
      const deleteBtn = e.target.closest(".delete-user-btn");
      if (deleteBtn) {
        const name = deleteBtn.getAttribute("data-name");
        await fetch("/api/users?name=" + encodeURIComponent(name), { method: "DELETE" });
        loadUsers();
      }
    });

    // load users when navigating
    viewLinks.forEach((link) => {
      link.addEventListener("click", () => {
        if (link.getAttribute("data-view") === "user-view") loadUsers();
      });
    });

    loginForm.addEventListener("submit", async (e) => {
      e.preventDefault();
      const username = loginForm.querySelector('input[type="text"]').value;
      const password = loginForm.querySelector('input[type="password"]').value;
      const r = await fetch("/api/login", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ username, password }) });
      if (r.ok) {
        showDashboard();
        showView("dashboard-view");
        loadUsers();
      } else {
        alert("Login failed");
      }
    });

    logoutLink.addEventListener("click", async (e) => {
      e.preventDefault();
      await fetch("/api/logout", { method: "POST" });
      showLogin();
    });

    viewLinks.forEach((link) => {
      link.addEventListener("click", (e) => {
        e.preventDefault();
        showView(link.getAttribute("data-view"));
      });
    });

    settingsForm.addEventListener("submit", (e) => {
      e.preventDefault();
      if (settingsPassword.value && settingsPassword.value !== settingsPasswordConfirm.value) {
        settingsMessage.textContent = "Passwords do not match.";
        settingsMessage.className = "message error";
        return;
      }
      const settings = {
        displayName: settingsDisplayName.value.trim(),
        email: settingsEmail.value.trim(),
        phone: settingsPhone.value.trim(),
        bio: settingsBio.value.trim(),
        avatar: settingsAvatarPreview.src,
        recoveryEmail: settingsRecoveryEmail.value.trim(),
        recoveryPhone: settingsRecoveryPhone.value.trim(),
        twoFactor: settings2fa.checked,
        emailNotifications: settingsEmailNotifications.checked,
        pushNotifications: settingsPushNotifications.checked,
        appearance: settingsAppearance.value,
        language: settingsLanguage.value,
        defaultPeriod: settingsDefaultPeriod.value,
      };
      if (!settings.displayName) return;
      if (settingsPassword.value) settings.passwordUpdatedAt = new Date().toISOString();
      try {
        localStorage.setItem("fcuBellSettings", JSON.stringify(settings));
      } catch {
        settings.avatar = "pfp.jpg";
        localStorage.setItem("fcuBellSettings", JSON.stringify(settings));
        settingsAvatarPreview.src = settings.avatar;
      }
      document.querySelector(".profile-section h2").textContent = settings.displayName;
      document.getElementById("profile-avatar").src = settings.avatar;
      document.body.dataset.appearance = settings.appearance;
      settingsPassword.value = "";
      settingsPasswordConfirm.value = "";
      settingsMessage.textContent = "Settings saved successfully.";
      settingsMessage.className = "message success";
    });

    async function uploadAudioFile(file) {
      const formData = new FormData();
      formData.append("file", file, file.name);
      try {
        const response = await fetch("/api/upload", { method: "POST", body: formData });
        return response.ok;
      } catch (error) {
        return false;
      }
    }


    const audioUploadFileInput = document.getElementById("audio-upload-file");
    const uploadAudioBtn = document.getElementById("upload-audio-btn");
    const uploadMessage = document.getElementById("upload-message");
    if (uploadAudioBtn) {
      uploadAudioBtn.addEventListener("click", async () => {
        const selectedFile = audioUploadFileInput?.files[0];
        if (!selectedFile) { if (uploadMessage) { uploadMessage.textContent = "Please choose a file first."; uploadMessage.className = "message error"; } return; }
        if (uploadMessage) { uploadMessage.textContent = "Uploading audio file..."; uploadMessage.className = "message"; }
        const uploaded = await uploadAudioFile(selectedFile);
        if (uploaded) {
          if (uploadMessage) { uploadMessage.textContent = `Uploaded ${selectedFile.name} to SD card.`; uploadMessage.className = "message success"; }
          loadAudioFiles();
        } else {
          if (uploadMessage) { uploadMessage.textContent = "Audio upload failed."; uploadMessage.className = "message error"; }
        }
      });
    }

    scheduleForm.addEventListener("submit", addOrUpdateSchedule);

    scheduleTimeInput.addEventListener("input", () => {
      scheduleTimeInput.value = scheduleTimeInput.value.replace(/[^0-9:]/g, "").slice(0, 5);
    });

    cancelEditBtn.addEventListener("click", resetScheduleForm);

    manualOverrideBtn.addEventListener("click", triggerManualOverride);
    emergencyBtn.addEventListener("click", triggerEmergencyDetection);
    backupBtn.addEventListener("click", backupAndRestore);

    document.addEventListener("click", (e) => {
      const button = e.target.closest("button");
      if (!button) return;

      const id = Number(button.getAttribute("data-id"));
      if (button.classList.contains("edit-btn")) {
        startEditSchedule(id);
      } else if (button.classList.contains("delete-btn")) {
        deleteSchedule(id);
      }
    });

    setInterval(updateDashboardStatus, 1000);
    renderSchedules();
    loadAudioFiles();
    updateDashboardStatus();
    loadSettings();
    document.querySelector(".profile-section h2").textContent = settingsDisplayName.value;

    if (localStorage.getItem("fcuBellLoggedIn") === "true") {
      showDashboard();
      showView("dashboard-view");
    } else {
      showLogin();
    }
  
