# PES-VCS — Version Control System (OS Lab)

**Name:** Pratham P Shetty
**SRN:** PES2UG24CS370

---

## 📌 Project Overview

PES-VCS is a simplified version control system implemented in C, inspired by the internal design of Git. It enables tracking of file changes, efficient storage of project snapshots, and maintenance of commit history using content-addressable storage.

This project reflects key concepts from Operating Systems and File Systems, focusing on how data is stored, organized, and retrieved efficiently.

---

## ⚙️ Features

* `pes init` — Initialize a new repository
* `pes add <file>` — Stage files for commit
* `pes status` — Display file status
* `pes commit -m "message"` — Create a commit
* `pes log` — Display commit history

---

## 🧠 Core Concepts

### 🔹 Content-Addressable Storage

All files are stored using SHA-256 hashing. Identical content results in identical hashes, ensuring deduplication and integrity.

### 🔹 Object Types

* **Blob:** Stores raw file data
* **Tree:** Represents directory structure and file hierarchy
* **Commit:** Stores snapshot metadata including parent reference and message

### 🔹 Index (Staging Area)

The index acts as an intermediate layer between the working directory and the repository, tracking files prepared for the next commit.

### 🔹 Commit Mechanism

Each commit represents a complete snapshot of the project. Commits are linked using parent pointers, forming a history chain.

---

## 🛠️ Technologies Used

* C Programming Language
* GCC Compiler
* Makefile
* Ubuntu (Linux Environment)

---

## 🚀 Build and Run

### Build

```bash
make
```

### Initialize Repository

```bash
./pes init
```

### Add Files

```bash
./pes add <filename>
```

### Commit Changes

```bash
./pes commit -m "commit message"
```

### View Commit History

```bash
./pes log
```

---

## 📂 Project Structure

```
.pes/
├── objects/          # Stores blobs, trees, commits
├── refs/
│   └── heads/
│       └── main      # Branch reference
├── index             # Staging area
└── HEAD              # Current branch pointer
```

---

## 🧪 Testing

The project includes test programs to validate different components such as object storage, tree construction, and full system integration.

---

## 📊 Learning Outcomes

* Understanding of version control system internals
* Implementation of hashing and storage mechanisms
* Knowledge of filesystem structures and data integrity
* Experience with low-level programming in C

---

## 💡 Conclusion

PES-VCS demonstrates how a version control system can be built using fundamental concepts of file systems and data structures. It highlights the efficiency and reliability achieved through content-based storage and structured object management.

---
