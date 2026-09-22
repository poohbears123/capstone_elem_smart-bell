(() => {
  document.querySelectorAll(".logout a").forEach((link) => {
    link.addEventListener("click", () => localStorage.removeItem("fcuBellLoggedIn"));
  });

  const scheduleStorageKey = "fcuBellSchedules";
  const defaultSchedules = [
    { id: 1, time: "07:45 AM", description: "Flag Ceremony", audio: "anthem.mp3" },
    { id: 2, time: "08:30 AM", description: "Start of Class", audio: "bell.mp3" },
    { id: 3, time: "10:00 AM", description: "Recess", audio: "recess.mp3" },
    { id: 4, time: "03:00 PM", description: "Dismissal", audio: "dismissal.mp3" }
  ];

  const readSchedules = () => {
    try {
      return JSON.parse(localStorage.getItem(scheduleStorageKey)) || defaultSchedules;
    } catch {
      return defaultSchedules;
    }
  };

  const saveSchedules = (schedules) => localStorage.setItem(scheduleStorageKey, JSON.stringify(schedules));

  const showMessage = (message) => {
    let messageElement = document.getElementById("page-message");
    if (!messageElement) {
      messageElement = document.createElement("p");
      messageElement.id = "page-message";
      messageElement.className = "message success";
      document.querySelector(".main-content").prepend(messageElement);
    }
    messageElement.textContent = message;
  };

  const scheduleBody = document.getElementById("standalone-schedule-body");
  if (scheduleBody) {
    let schedules = readSchedules();
    const form = document.getElementById("standalone-schedule-form");
    const timeInput = document.getElementById("standalone-time");
    const descriptionInput = document.getElementById("standalone-description");
    const audioInput = document.getElementById("standalone-audio");
    const audioFileInput = document.getElementById("standalone-audio-file");
    let editingId = null;

    const render = () => {
      scheduleBody.innerHTML = schedules.length ? schedules.map((item) => `
        <tr>
          <td>${item.time}</td><td>${item.description}</td><td>${item.audio}</td>
          <td><button type="button" class="edit-schedule" data-id="${item.id}">Edit</button>
          <button type="button" class="delete-schedule delete-btn" data-id="${item.id}">Delete</button></td>
        </tr>`).join("") : '<tr><td colspan="4" class="empty-state">No bell schedules yet.</td></tr>';
      document.getElementById("schedule-count").textContent = schedules.length;
      document.getElementById("next-bell").textContent = schedules.length ? `${schedules[0].time} - ${schedules[0].description}` : "No schedule set";
      saveSchedules(schedules);
    };

    form.addEventListener("submit", (event) => {
      event.preventDefault();
      const time = timeInput.value.trim();
      const description = descriptionInput.value.trim();
      const selectedFile = audioFileInput.files[0];
      const current = schedules.find((item) => item.id === editingId);
      const audio = selectedFile?.name || audioInput.value.trim() || current?.audio;
      if (!time || !description || !audio) {
        showMessage("Please provide a time, description, and audio file.");
        return;
      }
      const entry = { id: editingId || Date.now(), time, description, audio };
      schedules = editingId ? schedules.map((item) => item.id === editingId ? { ...item, ...entry } : item) : [...schedules, entry];
      saveSchedules(schedules);
      render();
      form.reset();
      editingId = null;
      showMessage("Schedule saved successfully.");
    });

    scheduleBody.addEventListener("click", (event) => {
      const button = event.target.closest("button");
      if (!button) return;
      const id = Number(button.dataset.id);
      const item = schedules.find((entry) => entry.id === id);
      if (button.classList.contains("edit-schedule")) {
        editingId = id;
        timeInput.value = item.time;
        descriptionInput.value = item.description;
        audioInput.value = item.audio;
        timeInput.focus();
        showMessage("Editing schedule entry.");
      } else if (button.classList.contains("delete-schedule")) {
        schedules = schedules.filter((entry) => entry.id !== id);
        saveSchedules(schedules);
        render();
        showMessage("Schedule deleted successfully.");
      }
    });
    document.getElementById("cancel-schedule").addEventListener("click", () => { form.reset(); editingId = null; });
    document.getElementById("add-schedule").addEventListener("click", () => {
      form.reset();
      editingId = null;
      timeInput.focus();
    });
    render();
  }

  document.querySelectorAll(".user-action").forEach((button) => {
    button.addEventListener("click", () => {
      const row = button.closest("tr");
      if (button.dataset.action === "approve") {
        row.querySelector(".status-badge").textContent = "Active";
        row.querySelector(".status-badge").className = "status-badge active";
        button.remove();
        showMessage("User approved successfully.");
      } else {
        showMessage(`Editing ${row.cells[0].textContent}.`);
      }
    });
  });
})();