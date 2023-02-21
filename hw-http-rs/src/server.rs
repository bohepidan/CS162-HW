const SEND_BUF_SIZE: usize = 1024;

use std::env;

use crate::args;

use crate::http;
use crate::http::*;
use crate::stats::*;

use clap::Parser;
use tokio::io::AsyncReadExt;
use tokio::io::AsyncWriteExt;
use tokio::net::TcpStream;
use tokio::net::TcpListener;
use tokio::fs::File;

use anyhow::Result;

pub fn main() -> Result<()> {
    // Configure logging
    // You can print logs (to stderr) using
    // `log::info!`, `log::warn!`, `log::error!`, etc.
    env_logger::Builder::new()
        .filter_level(log::LevelFilter::Info)
        .init();

    // Parse command line arguments
    let args = args::Args::parse();

    // Set the current working directory
    env::set_current_dir(&args.files)?;

    // Print some info for debugging
    log::info!("HTTP server initializing ---------");
    log::info!("Port:\t\t{}", args.port);
    log::info!("Num threads:\t{}", args.num_threads);
    log::info!("Directory:\t\t{}", &args.files);
    log::info!("----------------------------------");

    // Initialize a thread pool that starts running `listen`
    tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .worker_threads(args.num_threads)
        .build()?
        .block_on(listen(args.port))
}

async fn listen(port: u16) -> Result<()> {
    // Hint: you should call `handle_socket` in this function.
    let addr = format!("127.0.0.1:{}", port);
    let listener = TcpListener::bind(addr).await?;
    loop {
        let (socket, _) = listener.accept().await?;
        tokio::spawn(handle_socket(socket));
    }
}

// Handles a single connection via `socket`.
async fn handle_socket(mut socket: TcpStream) -> Result<()> {
    loop{
        //Read request from client
        socket.readable().await?;
        let req = match parse_request(&mut socket).await {
            Ok(n) => n,
            Err(ref e) if e.kind() == http::HttpError => {
                return Ok(());
            }
            _=> {
                log::warn!("Unexpected error:{}", e);
                continue
            }
        };
        
        //And resopnse.
        match req.method.as_str() {
            "GET" => ok_or_warn(response_get(&mut socket, req.path).await),
            _ => continue
        }
    }
}

async fn response_get(socket:&mut TcpStream, _path: String) -> Result<()> {
    let mut path = String::from(_path);
    path.insert(0, '.');

    let mut f = match File::open(&path).await {
        Ok(n) => n,
        Err(e) => match e.kind() {
            tokio::io::ErrorKind::NotFound => {
                //File Not Found
                ok_or_warn(response_not_found(socket).await);
                return Ok(());
            }
            _ => {
                log::warn!("Fopen err:{}", e);
                return Ok(());
            }
        }
    };

    start_response(socket, 200).await?;
    send_header(socket, "Content-Type", get_mime_type(path.as_str())).await?;
    send_header(socket, "Content-Length", format!("{}", f.metadata().await?.len()).as_str()).await?;
    end_headers(socket).await?;

    send_file(socket, &mut f).await?;

    Ok(())
}

//F is assumed to be opened.
async fn send_file(socket: &mut TcpStream, f: &mut File) -> Result<()> {
    let mut buf = [0; SEND_BUF_SIZE];
    loop {
        let n = f.read(&mut buf).await?;
        if n == 0{
            break;
        }

        socket.write_all(&buf[..n]).await?;
    }
    Ok(())
}

async fn response_not_found(socket:& mut TcpStream) -> Result<()> {
    start_response(socket, 404).await?;
    end_headers(socket).await?;
    Ok(())
}

fn ok_or_warn(res: Result<()>){
    match res {
        Ok(()) => {}
        Err(e) => log::warn!("statr_response err:{}", e)
    }
}

// You are free (and encouraged) to add other funtions to this file.
// You can also create your own modules as you see fit.
